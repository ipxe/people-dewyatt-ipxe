/*
 * Copyright (C) 2012 Daniel Wyatt <Daniel.Wyatt@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/netdevice.h>
#include <ipxe/ethernet.h>
#include <ipxe/if_ether.h>
#include <ipxe/iobuf.h>
#include <ipxe/malloc.h>
#include <ipxe/pci.h>
#include <ipxe/mii.h>
#include "bnx2.h"
#include "bnx2_fw.h"

#define SPEED_10		10
#define SPEED_100		100
#define SPEED_1000		1000
#define SPEED_2500		2500
#define DUPLEX_HALF		1
#define DUPLEX_FULL		2

/** @file
 *
 * bnx2 network driver
 *
 */

static void bnx2_free_mem ( struct bnx2_nic *bnx2 );

static uint32_t bnx2_reg_read_indirect ( struct bnx2_nic *bnx2,
										 uint32_t offset ) {
	uint32_t value;
	pci_write_config_dword ( bnx2->pci,
							 BNX2_PCICFG_REG_WINDOW_ADDRESS,
							 offset );
	pci_read_config_dword ( bnx2->pci, BNX2_PCICFG_REG_WINDOW, &value );
	return value;
}

static void bnx2_reg_write_indirect ( struct bnx2_nic *bnx2,
									  uint32_t offset,
									  uint32_t value ) {
	pci_write_config_dword ( bnx2->pci,
							 BNX2_PCICFG_REG_WINDOW_ADDRESS,
							 offset );
	pci_write_config_dword ( bnx2->pci, BNX2_PCICFG_REG_WINDOW, value );
}

static void bnx2_shmem_init ( struct bnx2_nic *bnx2 ) {
	uint32_t value;
	value = bnx2_reg_read_indirect ( bnx2, BNX2_SHM_HDR_SIGNATURE );
	if ( ( value & BNX2_SHM_HDR_SIGNATURE_SIG_MASK ) == BNX2_SHM_HDR_SIGNATURE_SIG )
		bnx2->shmem = bnx2_reg_read_indirect ( bnx2, BNX2_SHM_HDR_ADDR_0 );
	else
		bnx2->shmem = BNX2_HOST_VIEW_SHMEM_BASE;
}

static uint32_t bnx2_shmem_read ( struct bnx2_nic *bnx2, uint32_t offset ) {
	return bnx2_reg_read_indirect ( bnx2, bnx2->shmem + offset );
}

static void bnx2_shmem_write ( struct bnx2_nic *bnx2, uint32_t offset,
							   uint32_t value ) {
	bnx2_reg_write_indirect ( bnx2, bnx2->shmem + offset, value );
}

static void bnx2_set_mac_address ( struct bnx2_nic *bnx2,
								   struct net_device *netdev ) {
	uint32_t value;
	value = bnx2_shmem_read ( bnx2, BNX2_PORT_HW_CFG_MAC_UPPER );
	netdev->hw_addr[0] = ( uint8_t ) ( value >> 8 );
	netdev->hw_addr[1] = ( uint8_t ) value;
	value = bnx2_shmem_read ( bnx2, BNX2_PORT_HW_CFG_MAC_LOWER );
	netdev->hw_addr[2] = ( uint8_t ) ( value >> 24 );
	netdev->hw_addr[3] = ( uint8_t ) ( value >> 16 );
	netdev->hw_addr[4] = ( uint8_t ) ( value >> 8 );
	netdev->hw_addr[5] = ( uint8_t ) value;

	/* We use the lower 3 bytes as the back-off seed */
	value = netdev->hw_addr[5] + netdev->hw_addr[4] + netdev->hw_addr[3];
	value &= BNX2_EMAC_BACKOFF_SEED_EMAC_BACKOFF_SEED;
	writel ( value, bnx2->regs + BNX2_EMAC_BACKOFF_SEED );
}

static int bnx2_alloc_mem ( struct bnx2_nic *bnx2 ) {
	bnx2->status_blk = malloc_dma ( sizeof ( struct bnx2_status_block ),
									sizeof ( struct bnx2_status_block ) );
	bnx2->tx_ring.desc = malloc_dma ( sizeof ( struct bnx2_tx_bd ) * TX_DESC_CNT,
									  sizeof ( struct bnx2_tx_bd ) );
	bnx2->rx_ring.desc = malloc_dma ( sizeof ( struct bnx2_rx_bd ) * RX_DESC_CNT,
									  sizeof ( struct bnx2_rx_bd ) );
	if ( !bnx2->status_blk || !bnx2->tx_ring.desc || !bnx2->rx_ring.desc ) {
		/* These are zeroed earlier so this is safe */
		bnx2_free_mem ( bnx2 );
		return -ENOMEM;
	}
	memset ( bnx2->status_blk, 0, sizeof ( struct bnx2_status_block ) );
	memset ( bnx2->tx_ring.desc, 0, sizeof ( struct bnx2_tx_bd ) * TX_DESC_CNT );
	memset ( bnx2->rx_ring.desc, 0, sizeof ( struct bnx2_rx_bd ) * RX_DESC_CNT );
	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 ) {
		int i;
		bnx2->ctx_pages = 0x2000 / BCM_PAGE_SIZE;
		if ( bnx2->ctx_pages == 0 )
			bnx2->ctx_pages = 1;

		for ( i = 0; i < bnx2->ctx_pages; i++ ) {
			bnx2->ctx_blk[i] = malloc_dma ( BCM_PAGE_SIZE, BCM_PAGE_SIZE );
			if ( !bnx2->ctx_blk[i] ) {
				bnx2_free_mem ( bnx2 );
				return -ENOMEM;
			}
		}
	}
	return 0;
}

static void bnx2_free_mem ( struct bnx2_nic *bnx2 ) {
	free_dma ( bnx2->status_blk, sizeof ( struct bnx2_status_block ) );
	bnx2->status_blk = NULL;
	free_dma ( bnx2->tx_ring.desc, sizeof ( struct bnx2_tx_bd ) * TX_DESC_CNT );
	bnx2->tx_ring.desc = NULL;
	free_dma ( bnx2->rx_ring.desc, sizeof ( struct bnx2_rx_bd ) * RX_DESC_CNT );
	bnx2->rx_ring.desc = NULL;
	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 ) {
		int i;
		
		for ( i = 0; i < bnx2->ctx_pages; i++ ) {
			free_dma ( bnx2->ctx_blk[i], BCM_PAGE_SIZE );
			bnx2->ctx_blk[i] = NULL;
		}
	}
}

static int bnx2_fw_sync ( struct bnx2_nic *bnx2, uint32_t message_data ) {
	int i;
	uint32_t value;

	bnx2->fw_write_sequence++;
	message_data |= bnx2->fw_write_sequence;
	bnx2_shmem_write ( bnx2, BNX2_DRV_MB, message_data );

	/* Wait for acknowledgement */
	for ( i = 0; i < BNX2_FW_ACK_TIMEOUT_MS; i++ ) {
		value = bnx2_shmem_read ( bnx2, BNX2_FW_MB );
		if ( ( value & BNX2_FW_MSG_ACK ) == ( message_data & BNX2_DRV_MSG_SEQ ) )
			break;

		mdelay ( 1 );
	}
	if ( ( value & BNX2_FW_MSG_ACK ) != ( message_data & BNX2_DRV_MSG_SEQ ) &&
		 ( value & BNX2_DRV_MSG_DATA ) != BNX2_DRV_MSG_DATA_WAIT0 ) {
		message_data &= ~BNX2_DRV_MSG_CODE;
		message_data |= BNX2_DRV_MSG_CODE_FW_TIMEOUT;

		bnx2_shmem_write ( bnx2, BNX2_DRV_MB, message_data );
		return -EBUSY;
	}
	if ( ( value & BNX2_FW_MSG_STATUS_MASK ) != BNX2_FW_MSG_STATUS_OK )
		return -EIO;

	return 0;
}

static void bnx2_context_write ( struct bnx2_nic *bnx2,
								  uint32_t cid_addr,
								  uint32_t offset,
								  uint32_t value ) {
	offset += cid_addr;
	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 ) {
		int i;
		writel ( value, bnx2->regs + BNX2_CTX_CTX_DATA );
		writel ( offset | BNX2_CTX_CTRL_WRITE_REQ, bnx2->regs + BNX2_CTX_CTRL );
		for ( i = 0; i < 5; i++ ) {
			value = readl ( bnx2->regs + BNX2_CTX_CTRL );
			if ( ! ( value & BNX2_CTX_CTRL_WRITE_REQ ) )
				break;

			udelay ( 5 );
		}
	} else {
		writel ( offset, bnx2->regs + BNX2_CTX_DATA_ADR );
		writel ( value, bnx2->regs + BNX2_CTX_DATA );
	}
}

static int bnx2_init_context_5709 ( struct bnx2_nic *bnx2 ) {
	uint32_t value;
	int i;

	value = BNX2_CTX_COMMAND_ENABLED | BNX2_CTX_COMMAND_MEM_INIT |
			( ( BCM_PAGE_BITS - 8 ) << 16 );
	writel ( value, bnx2->regs + BNX2_CTX_COMMAND );
	for ( i = 0; i < 10; i++ ) {
		value = readl ( bnx2->regs + BNX2_CTX_COMMAND );
		/* Wait for completion */
		if ( ! ( value & BNX2_CTX_COMMAND_MEM_INIT ) )
			break;

		udelay ( 2 );
	}
	if ( value & BNX2_CTX_COMMAND_MEM_INIT )
		return -ETIMEDOUT;

	for ( i = 0; i < bnx2->ctx_pages; i++ ) {
		int j;
		memset ( bnx2->ctx_blk[i], 0, BCM_PAGE_SIZE );

		writel ( ( virt_to_bus ( bnx2->ctx_blk[i] ) & 0xffffffff ) |
					BNX2_CTX_HOST_PAGE_TBL_DATA0_VALID,
				bnx2->regs + BNX2_CTX_HOST_PAGE_TBL_DATA0 );
		writel (  ( uint64_t ) ( virt_to_bus ( bnx2->ctx_blk[i] ) ) >> 32,
				  bnx2->regs + BNX2_CTX_HOST_PAGE_TBL_DATA1 );
		writel ( i | BNX2_CTX_HOST_PAGE_TBL_CTRL_WRITE_REQ,
				 bnx2->regs + BNX2_CTX_HOST_PAGE_TBL_CTRL );
		for ( j = 0; j < 10; j++ ) {
			value = readl ( bnx2->regs + BNX2_CTX_HOST_PAGE_TBL_CTRL );
			if ( ! ( value & BNX2_CTX_HOST_PAGE_TBL_CTRL_WRITE_REQ ) )
				break;

			udelay ( 5 );
		}
		if ( value & BNX2_CTX_HOST_PAGE_TBL_CTRL_WRITE_REQ )
			return -ETIMEDOUT;
	}
	return 0;
}

static void bnx2_init_context ( struct bnx2_nic *bnx2 ) {
	int i;
	uint32_t vcid;
	uint32_t vcid_addr;
	uint32_t pcid_addr;
	uint32_t offset;

	/* Initialize the 96 Quick CIDs */
	for ( vcid = 0; vcid < 96; vcid++ ) {
		if ( CHIP_ID ( bnx2->misc_id ) == CHIP_ID_5706_A0 ) {
			uint32_t new_vcid;
			vcid_addr = GET_PHY_CTX_ID_ADDR ( vcid );
			if ( vcid & 0x8 )
				new_vcid = 0x60 + ( vcid & 0xf0 ) + ( vcid & 0x7 );
			else
				new_vcid = vcid;

			pcid_addr = GET_PHY_CTX_ID_ADDR ( new_vcid );
		} else {
			vcid_addr = GET_CTX_ID_ADDR ( vcid );
			pcid_addr = vcid_addr;
		}
		for ( i = 0; i < ( CTX_SIZE / PHY_CTX_SIZE ); i++ ) {
			vcid_addr += ( i << PHY_CTX_SHIFT );
			pcid_addr += ( i << PHY_CTX_SHIFT );
			writel ( vcid_addr, bnx2->regs + BNX2_CTX_VIRT_ADDR );
			writel ( pcid_addr, bnx2->regs + BNX2_CTX_PAGE_TBL );
			for ( offset = 0; offset < PHY_CTX_SIZE; offset += 4 ) {
				bnx2_context_write ( bnx2, vcid_addr, offset, 0 );
			}
		}

	}
}

static uint32_t rv2p_fw_fixup ( int idx, uint32_t rv2p_code )
{
	switch ( idx ) {
	case RV2P_P1_FIXUP_PAGE_SIZE_IDX:
		rv2p_code &= ~RV2P_BD_PAGE_SIZE_MASK;
		rv2p_code |= RV2P_BD_PAGE_SIZE;
		break;
	}
	return rv2p_code;
}

static void bnx2_load_rv2p_proc ( struct bnx2_nic *bnx2,
								  const uint32_t *fw,
								  struct bnx2_rv2p_fw_file_entry *entry,
								  int proc ) {
	uint32_t length;
	uint32_t offset;
	uint32_t address;
	uint32_t command;
	uint32_t value;
	uint32_t *code;
	unsigned int i;

	command = ( proc == RV2P_PROC1 ) ?
			  BNX2_RV2P_PROC1_ADDR_CMD_RDWR :
			  BNX2_RV2P_PROC2_ADDR_CMD_RDWR;
	address = ( proc == RV2P_PROC1 ) ?
			  BNX2_RV2P_PROC1_ADDR_CMD :
			  BNX2_RV2P_PROC2_ADDR_CMD;
	length = be32_to_cpu ( entry->rv2p.len );
	offset = be32_to_cpu ( entry->rv2p.offset );
	code = ( uint32_t * ) ( ( ( uint8_t * ) fw ) + offset );
	for ( i = 0; i < length; i += 8 ) {
		writel ( be32_to_cpu ( *code++ ), bnx2->regs + BNX2_RV2P_INSTR_HIGH );
		writel ( be32_to_cpu ( *code++ ), bnx2->regs + BNX2_RV2P_INSTR_LOW );
		value = ( i / 8 ) | command;
		writel ( value, bnx2->regs + address );
	}
	code = ( uint32_t * ) ( ( ( uint8_t * ) fw ) + offset );
	for ( i = 0; i < 8; i++ ) {
		uint32_t location;
		uint32_t c;
		location = be32_to_cpu ( entry->fixup[i] );
		if ( location && ( ( location * 4 ) < length ) ) {
			c = be32_to_cpu ( * ( code + location - 1 ) );
			writel ( c, bnx2->regs + BNX2_RV2P_INSTR_HIGH );
			c = be32_to_cpu ( * ( code + location ) );
			c = rv2p_fw_fixup ( i, c );
			writel ( c, bnx2->regs + BNX2_RV2P_INSTR_LOW );
			value = ( location / 2 ) | command;
			writel ( value, bnx2->regs + address );
		}
	}
	command = ( proc == RV2P_PROC1 ) ?
				BNX2_RV2P_COMMAND_PROC1_RESET :
				BNX2_RV2P_COMMAND_PROC2_RESET;
	writel ( command, bnx2->regs + BNX2_RV2P_COMMAND );
}

static void bnx2_load_rv2p_firmware ( struct bnx2_nic *bnx2,
									  const uint32_t *fw,
									  struct bnx2_rv2p_fw_file *fw_file ) {
	bnx2_load_rv2p_proc ( bnx2, fw, &fw_file->proc1, RV2P_PROC1 );
	bnx2_load_rv2p_proc ( bnx2, fw, &fw_file->proc2, RV2P_PROC2 );
}

static void bnx2_load_mips_firmware_section ( struct bnx2_nic *bnx2,
											  const struct cpu_reg *cpu,
											  const uint32_t *fw,
											  struct bnx2_fw_file_section *section ) {
	uint32_t address, length, file_offset;
	uint32_t offset;
	uint32_t *data;
	address = be32_to_cpu ( section->addr );
	length = be32_to_cpu ( section->len );
	file_offset = be32_to_cpu ( section->offset );
	data = ( uint32_t * ) ( ( ( uint8_t * ) fw ) + file_offset );
	offset = cpu->spad_base + ( address - BNX2_MIPS_VIEW_BASE );
	if ( length ) {
		unsigned int i;
		for ( i = 0; i < length / 4; i++ ) {
			bnx2_reg_write_indirect ( bnx2, offset, be32_to_cpu ( data[i] ) );
			offset += 4;
		}
	}
}

static void bnx2_load_mips_firmware_entry ( struct bnx2_nic *bnx2,
											const struct cpu_reg *cpu,
											const uint32_t *fw,
											struct bnx2_mips_fw_file_entry *entry ) {
	/* We assume the CPU is already halted from the reset */
	uint32_t value;
	value = bnx2_reg_read_indirect ( bnx2, cpu->mode );
	value |= BNX2_CPU_MODE_SOFT_HALT;
	bnx2_reg_write_indirect ( bnx2, cpu->mode, value );
	bnx2_reg_write_indirect ( bnx2, cpu->state, 0xffffff );
	bnx2_load_mips_firmware_section ( bnx2, cpu, fw, &entry->text );
	bnx2_load_mips_firmware_section ( bnx2, cpu, fw, &entry->data );
	bnx2_load_mips_firmware_section ( bnx2, cpu, fw, &entry->rodata );
	/* Clear pre-fetch instruction */
	bnx2_reg_write_indirect ( bnx2, cpu->inst, 0 );
	/* Set the program counter up */
	value = be32_to_cpu ( entry->start_addr );
	bnx2_reg_write_indirect ( bnx2, cpu->pc, value );
	/* Unset halt bit */
	value = bnx2_reg_read_indirect ( bnx2, cpu->mode );
	value &= ~BNX2_CPU_MODE_SOFT_HALT;
	/* Clear CPU states */
	bnx2_reg_write_indirect ( bnx2, cpu->state, 0xffffff );
	bnx2_reg_write_indirect ( bnx2, cpu->mode, value );
}

static void bnx2_load_mips_firmware ( struct bnx2_nic *bnx2,
									  const uint32_t *fw,
									  struct bnx2_mips_fw_file *fw_file ) {
	bnx2_load_mips_firmware_entry ( bnx2, &cpu_reg_rxp, fw, &fw_file->rxp );
	bnx2_load_mips_firmware_entry ( bnx2, &cpu_reg_txp, fw, &fw_file->txp );
	bnx2_load_mips_firmware_entry ( bnx2, &cpu_reg_tpat, fw, &fw_file->tpat );
	bnx2_load_mips_firmware_entry ( bnx2, &cpu_reg_com, fw, &fw_file->com );
	bnx2_load_mips_firmware_entry ( bnx2, &cpu_reg_cp, fw, &fw_file->cp );
}

static void bnx2_load_firmware ( struct bnx2_nic *bnx2 ) {
	const uint32_t *rv2p_firmware;
	const uint32_t *mips_firmware;

	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 ) {
		if ( CHIP_ID ( bnx2->misc_id ) == CHIP_ID_5709_A0 ||
			 CHIP_ID ( bnx2->misc_id ) == CHIP_ID_5709_A1 )
			rv2p_firmware = bnx2_rv2p_09ax_firmware;
		else
			rv2p_firmware = bnx2_rv2p_09_firmware;

		mips_firmware = bnx2_mips_09_firmware;
	} else {
		rv2p_firmware = bnx2_rv2p_06_firmware;
		mips_firmware = bnx2_mips_06_firmware;
	}
	bnx2_load_rv2p_firmware ( bnx2, rv2p_firmware,
								   ( struct bnx2_rv2p_fw_file * ) rv2p_firmware );
	bnx2_load_mips_firmware ( bnx2, mips_firmware,
								   ( struct bnx2_mips_fw_file * ) mips_firmware );
}

static int bnx2_reset_chip ( struct bnx2_nic *bnx2 ) {
	uint32_t value;
	int rc;
	bnx2_shmem_write ( bnx2, BNX2_DRV_RESET_SIGNATURE,
							 BNX2_DRV_RESET_SIGNATURE_MAGIC );

	value = readl ( bnx2->regs + BNX2_MISC_COMMAND );
	value |= BNX2_MISC_COMMAND_CORE_RESET;
	writel ( value, bnx2->regs + BNX2_MISC_COMMAND );
	udelay ( 100 );
	value = readl ( bnx2->regs + BNX2_MISC_COMMAND );
	udelay ( 100 );
	writel ( BNX2_PCICFG_MISC_CONFIG_TARGET_MB_WORD_SWAP | BNX2_PCICFG_MISC_CONFIG_REG_WINDOW_ENA, bnx2->regs + BNX2_PCICFG_MISC_CONFIG );
	if ( ( rc = bnx2_fw_sync ( bnx2, BNX2_DRV_MSG_DATA_WAIT1 |
									 BNX2_DRV_MSG_CODE_RESET ) ) )
		return rc;

	return 0;
}

static void bnx2_set_rx_mode ( struct bnx2_nic *bnx2 ) {
	uint32_t sort_mode;
	uint32_t value;
	uint8_t *mac_addr = bnx2->netdev->hw_addr;
	int i;
	for ( i = 0; i < NUM_MC_HASH_REGISTERS; i++ )
		writel ( 0xffffffff, bnx2->regs + BNX2_EMAC_MULTICAST_HASH0
							 + ( i * 4 ) );


	writel ( BNX2_EMAC_RX_MODE_SORT_MODE, bnx2->regs + BNX2_EMAC_RX_MODE );
	writel ( 0, bnx2->regs + BNX2_RPM_SORT_USER0 );
	sort_mode = BNX2_RPM_SORT_USER0_BC_EN | BNX2_RPM_SORT_USER0_MC_EN | 1;
	writel ( sort_mode, bnx2->regs + BNX2_RPM_SORT_USER0 );
	writel ( sort_mode | BNX2_RPM_SORT_USER0_ENA, bnx2->regs + BNX2_RPM_SORT_USER0 );

	value = ( mac_addr[0] << 8 ) | mac_addr[1];
	writel ( value, bnx2->regs +  BNX2_EMAC_MAC_MATCH0 );

	value = ( mac_addr[2] << 24 ) | ( mac_addr[3] << 16 ) |
			( mac_addr[4] << 8 ) | mac_addr[5];
	writel ( value, bnx2->regs + BNX2_EMAC_MAC_MATCH1 );
}

static int bnx2_init_chip ( struct bnx2_nic *bnx2 ) {
	uint32_t value;
	int rc;

	value = BNX2_DMA_CONFIG_DATA_BYTE_SWAP |
			BNX2_DMA_CONFIG_DATA_WORD_SWAP |
#if __BYTE_ORDER == __BIG_ENDIAN
			BNX2_DMA_CONFIG_CNTL_BYTE_SWAP |
#endif
			BNX2_DMA_CONFIG_CNTL_WORD_SWAP |
			//BNX2_DMA_CONFIG_CNTL_PING_PONG_DMA |
			( 2 << 20 ) |	/* PCI_CLK_CMP_BITS */
			( 1 << 11 ) |	/* CNTL_PCI_COMP_DLY */
			( 1 << 12 ) |	/* DMA read channels */
			( 1 << 16 );	/* DMA write channels */
	writel ( value, bnx2->regs + BNX2_DMA_CONFIG );

	writel ( BNX2_MISC_ENABLE_SET_BITS_CONTEXT_ENABLE |
			 BNX2_MISC_ENABLE_SET_BITS_HOST_COALESCE_ENABLE |
			 BNX2_MISC_ENABLE_SET_BITS_RX_V2P_ENABLE,
			 bnx2->regs + BNX2_MISC_ENABLE_SET_BITS );
	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 )
		bnx2_init_context_5709 ( bnx2 );
	else
		bnx2_init_context ( bnx2 );

	bnx2_load_firmware ( bnx2 );

	value = ( BCM_PAGE_BITS - 8 ) << 24;
	writel ( value, bnx2->regs + BNX2_RV2P_CONFIG );

	writel ( BNX2_EMAC_ATTENTION_ENA_LINK, bnx2->regs + BNX2_EMAC_ATTENTION_ENA );
	writel ( ( uint64_t ) virt_to_phys ( bnx2->status_blk ) & 0xffffffff, bnx2->regs + BNX2_HC_STATUS_ADDR_L );
	writel ( ( uint64_t ) virt_to_phys ( bnx2->status_blk ) >> 32, bnx2->regs + BNX2_HC_STATUS_ADDR_H );
	writel ( STATUS_ATTN_BITS_LINK_STATE, bnx2->regs + BNX2_HC_ATTN_BITS_ENABLE );

	bnx2_set_rx_mode ( bnx2 );

	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 ) {
		value = readl ( bnx2->regs + BNX2_MISC_NEW_CORE_CTL );
		value |= BNX2_MISC_NEW_CORE_CTL_DMA_ENABLE;
		writel ( value, bnx2->regs + BNX2_MISC_NEW_CORE_CTL );
	}

	if ( ( rc = bnx2_fw_sync ( bnx2, BNX2_DRV_MSG_DATA_WAIT2 | BNX2_DRV_MSG_CODE_RESET ) ) )
		return rc;

	writel ( BNX2_MISC_ENABLE_DEFAULT, bnx2->regs + BNX2_MISC_ENABLE_SET_BITS );
	readl ( bnx2->regs + BNX2_MISC_ENABLE_SET_BITS );
	writel ( BNX2_HC_COMMAND_ENABLE | BNX2_HC_COMMAND_COAL_NOW_WO_INT, bnx2->regs + BNX2_HC_COMMAND );
	udelay ( 20 );
	bnx2->hc_cmd = readl ( bnx2->regs + BNX2_HC_COMMAND );
	return 0;
}

static void bnx2_init_tx_context ( struct bnx2_nic *bnx2 ) {
	uint32_t value;
	uint32_t offset0, offset1, offset2, offset3;

	if ( CHIP_NUM ( bnx2->misc_id ) == CHIP_NUM_5709 ) {
		offset0 = BNX2_L2CTX_TYPE_XI;
		offset1 = BNX2_L2CTX_CMD_TYPE_XI;
		offset2 = BNX2_L2CTX_TBDR_BHADDR_HI_XI;
		offset3 = BNX2_L2CTX_TBDR_BHADDR_LO_XI;
	} else {
		offset0 = BNX2_L2CTX_TYPE;
		offset1 = BNX2_L2CTX_CMD_TYPE;
		offset2 = BNX2_L2CTX_TBDR_BHADDR_HI;
		offset3 = BNX2_L2CTX_TBDR_BHADDR_LO;
	}
	value = BNX2_L2CTX_TYPE_TYPE_L2 | BNX2_L2CTX_TYPE_SIZE_L2;
	bnx2_context_write ( bnx2, TX_CID_ADDR, offset0, value );

	value = BNX2_L2CTX_CMD_TYPE_TYPE_L2 | ( 8 << 16 );
	bnx2_context_write ( bnx2, TX_CID_ADDR, offset1, value );

	value = 0;
	bnx2_context_write ( bnx2, TX_CID_ADDR, offset2, value );

	value = virt_to_bus ( bnx2->tx_ring.desc ) & 0xffffffff;
	bnx2_context_write ( bnx2, TX_CID_ADDR, offset3, value );
}

static void bnx2_init_rx_context ( struct bnx2_nic *bnx2 ) {
	uint32_t value;
	value = BNX2_L2CTX_CTX_TYPE_CTX_BD_CHN_TYPE_VALUE |
			BNX2_L2CTX_CTX_TYPE_SIZE_L2 |
			( 2 << 8 ) /* bd_pre_read */ ;
	bnx2_context_write ( bnx2, RX_CID_ADDR, BNX2_L2CTX_CTX_TYPE, value );

	bnx2_context_write ( bnx2, RX_CID_ADDR, BNX2_L2CTX_NX_BDHADDR_HI, 0 );

	value = virt_to_bus ( bnx2->rx_ring.desc ) & 0xffffffff;
	bnx2_context_write ( bnx2, RX_CID_ADDR, BNX2_L2CTX_NX_BDHADDR_LO, value );
}

static void bnx2_init_tx_ring ( struct bnx2_nic *bnx2 ) {
	struct bnx2_tx_ring_info *txr = &bnx2->tx_ring;
	struct bnx2_tx_bd *txbd;
	unsigned int i;

	txbd = &txr->desc[MAX_TX_DESC_CNT];
	txbd->haddr_hi = 0;
	txbd->haddr_lo = virt_to_bus ( txr->desc );
	for ( i = 0; i < TX_DESC_CNT; i++ ) {
		txbd = &txr->desc[i];
		txbd->flags = TX_BD_FLAGS_START | TX_BD_FLAGS_END;
	}
	bnx2_init_tx_context ( bnx2 );
}

static void bnx2_init_rx_ring ( struct bnx2_nic *bnx2 ) {
	struct bnx2_rx_ring_info *rxr = &bnx2->rx_ring;
	struct bnx2_rx_bd *rxbd;
	int i;
	for ( i = 0; i < ( int ) MAX_RX_DESC_CNT; i++ ) {
		rxbd = &rxr->desc[i];
		rxbd->flags = RX_BD_FLAGS_START | RX_BD_FLAGS_END;
		rxbd->len = RX_BUF_USE_SIZE;
	}
	rxbd = &rxr->desc[MAX_RX_DESC_CNT];
	rxbd->haddr_hi = 0;
	rxbd->haddr_lo = ( uint64_t ) virt_to_bus ( rxr->desc ) & 0xffffffff;

	bnx2_init_rx_context ( bnx2 );
}

static int bnx2_reset_nic ( struct bnx2_nic *bnx2 ) {
	int rc;
	if ( ( rc = bnx2_reset_chip ( bnx2 ) ) != 0 )
		return rc;

	if ( ( rc = bnx2_init_chip ( bnx2 ) ) != 0 )
		return rc;

	bnx2_init_tx_ring ( bnx2 );
	bnx2_init_rx_ring ( bnx2 );
	return 0;
}

static void bnx2_setup_copper_phy ( struct bnx2_nic *bnx2 ) {
	mii_write ( &bnx2->mii, MII_ADVERTISE, ADVERTISE_ALL | ADVERTISE_CSMA );
	mii_write ( &bnx2->mii, MII_CTRL1000,
				ADVERTISE_1000HALF | ADVERTISE_1000FULL );
	mii_write ( &bnx2->mii, MII_BMCR, BMCR_ANRESTART | BMCR_ANENABLE );
}

static void bnx2_setup_phy ( struct bnx2_nic *bnx2 ) {
	bnx2_setup_copper_phy ( bnx2 );
}

static void bnx2_init_phy ( struct bnx2_nic *bnx2 ) {
	bnx2_setup_phy ( bnx2 );
}

static void bnx2_set_mac_link ( struct bnx2_nic *bnx2, int speed, int duplex ) {
	uint32_t emac_mode = readl ( bnx2->regs + BNX2_EMAC_MODE );
	switch ( speed ) {
		case SPEED_10:
			if ( CHIP_NUM ( bnx2->misc_id ) != CHIP_NUM_5706 )
				emac_mode |= BNX2_EMAC_MODE_PORT_MII_10;

			/* fall through */
		case SPEED_100:
			emac_mode |= BNX2_EMAC_MODE_PORT_MII;
			break;
		case SPEED_2500:
			emac_mode |= BNX2_EMAC_MODE_25G;
			/* fall through */
		case SPEED_1000:
			emac_mode |= BNX2_EMAC_MODE_PORT_GMII;
			break;
	}
	if ( duplex == DUPLEX_HALF )
		emac_mode |= BNX2_EMAC_MODE_HALF_DUPLEX;

	writel ( emac_mode, bnx2->regs + BNX2_EMAC_MODE );
}

static void bnx2_copper_linkup ( struct bnx2_nic *bnx2 ) {
	uint32_t local_adv, remote_adv, common;
	uint32_t speed = 0, duplex = 0;

	local_adv = mii_read ( &bnx2->mii, MII_CTRL1000 );
	remote_adv = mii_read ( &bnx2->mii, MII_STAT1000 );
	common = local_adv & ( remote_adv >> 2 );
	if ( common & ADVERTISE_1000FULL ) {
		speed = SPEED_1000;
		duplex = DUPLEX_FULL;
	} else if ( common & ADVERTISE_1000HALF ) {
		speed = SPEED_1000;
		duplex = DUPLEX_HALF;
	} else {
		local_adv = mii_read ( &bnx2->mii, MII_ADVERTISE );
		remote_adv = mii_read ( &bnx2->mii, MII_LPA );
		common = local_adv & remote_adv;
		if ( common & ADVERTISE_100FULL ) {
			speed = SPEED_100;
			duplex = DUPLEX_FULL;
		} else if ( common & ADVERTISE_100HALF ) {
			speed = SPEED_100;
			duplex = DUPLEX_HALF;
		} else if ( common & ADVERTISE_10FULL ) {
			speed = SPEED_10;
			duplex = DUPLEX_FULL;
		} else if ( common & ADVERTISE_10HALF ) {
			speed = SPEED_10;
			duplex = DUPLEX_HALF;
		} else {
			//fail
		}
	}
	bnx2_set_mac_link ( bnx2, speed, duplex );
}

static void bnx2_set_link ( struct bnx2_nic *bnx2 ) {
	bnx2_copper_linkup ( bnx2 );
}

static int bnx2_init_nic ( struct bnx2_nic *bnx2 ) {
	int rc;

	if ( ( rc = bnx2_reset_nic ( bnx2 ) ) != 0 )
		return rc;

	bnx2_init_phy ( bnx2 );
	return 0;
}

/******************************************************************************
 *
 * MII interface
 *
 ******************************************************************************
 */

/**
 * Read from MII register
 *
 * @v mii		MII interface
 * @v reg		Register address
 * @ret value		Data read, or negative error
 */
static int bnx2_mii_read ( struct mii_interface *mii, unsigned int reg ) {
	struct bnx2_nic *bnx2 =
		container_of ( mii, struct bnx2_nic, mii );
	uint32_t value;
	int i;

	/* Check to see if there's a pending transaction */
	value = readl ( bnx2->regs + BNX2_EMAC_MDIO_COMM );
	if ( value & BNX2_EMAC_MDIO_COMM_START_BUSY )
		return -EBUSY;

	value = ( bnx2->phy_addr << 21 ) | ( reg << 16 ) |
			BNX2_EMAC_MDIO_COMM_COMMAND_READ |
			BNX2_EMAC_MDIO_COMM_START_BUSY;
	writel ( value, bnx2->regs + BNX2_EMAC_MDIO_COMM );
	for ( i = 0; i < 50; i++ ) {
		value = readl ( bnx2->regs + BNX2_EMAC_MDIO_COMM );
		if ( ! ( value & BNX2_EMAC_MDIO_COMM_START_BUSY ) ) {
			udelay ( 5 );
			value = readl ( bnx2->regs + BNX2_EMAC_MDIO_COMM );
			break;
		}
		udelay ( 10 );
	}
	if ( value & BNX2_EMAC_MDIO_COMM_START_BUSY )
		value = -EBUSY;
	else if ( value & BNX2_EMAC_MDIO_COMM_FAIL )
		value = -EIO;

	return value;
}

/**
 * Write to MII register
 *
 * @v mii		MII interface
 * @v reg		Register address
 * @v data		Data to write
 * @ret rc		Return status code
 */
static int bnx2_mii_write ( struct mii_interface *mii, unsigned int reg,
				unsigned int data) {
	struct bnx2_nic *bnx2 =
		container_of ( mii, struct bnx2_nic, mii );
	uint32_t value;
	int i;
	int rc;

	/* Check to see if there's a pending transaction */
	value = readl ( bnx2->regs + BNX2_EMAC_MDIO_COMM );
	if ( value & BNX2_EMAC_MDIO_COMM_START_BUSY )
		return -EBUSY;

	value = ( bnx2->phy_addr << 21 ) | ( reg << 16 ) | data |
			BNX2_EMAC_MDIO_COMM_COMMAND_WRITE |
			BNX2_EMAC_MDIO_COMM_START_BUSY;
	writel ( value, bnx2->regs + BNX2_EMAC_MDIO_COMM );

	for ( i = 0; i < 50; i++ ) {
		value = readl ( bnx2->regs + BNX2_EMAC_MDIO_COMM );
		if ( ! ( value & BNX2_EMAC_MDIO_COMM_START_BUSY ) ) {
				udelay ( 5 );
				break;
		}
		udelay ( 10 );
	}
	if ( value & BNX2_EMAC_MDIO_COMM_START_BUSY )
		rc = -EBUSY;
	else if ( value & BNX2_EMAC_MDIO_COMM_FAIL )
		rc = -EIO;
	else
		rc = 0;

	return rc;
}

/** bnx2 MII operations */
static struct mii_operations bnx2_mii_operations = {
	.read = bnx2_mii_read,
	.write = bnx2_mii_write,
};

/******************************************************************************
 *
 * Device reset
 *
 ******************************************************************************
 */

/**
 * Reset hardware
 *
 * @v bnx2		bnx2 device
 * @ret rc		Return status code
 */
static int bnx2_reset ( struct bnx2_nic *bnx2 ) {
	uint32_t value;

	writel ( BNX2_PCICFG_MISC_CONFIG_REG_WINDOW_ENA |
			 BNX2_PCICFG_MISC_CONFIG_TARGET_MB_WORD_SWAP,
			 bnx2->regs + BNX2_PCICFG_MISC_CONFIG );

	bnx2->misc_id = readl ( bnx2->regs + BNX2_MISC_ID );
	bnx2_shmem_init ( bnx2 );
	value = bnx2_shmem_read ( bnx2, BNX2_DEV_INFO_SIGNATURE );
	if ( ( value & BNX2_DEV_INFO_SIGNATURE_MAGIC_MASK ) !=
		 BNX2_DEV_INFO_SIGNATURE_MAGIC ) {
		DBGC ( bnx2, "BNX2 %p firmware is not running\n", bnx2 );
		return -EIO;
	}
	bnx2->phy_addr = 1;
	return 0;
}

/******************************************************************************
 *
 * Link state
 *
 ******************************************************************************
 */

/**
 * Check link state
 *
 * @v netdev		Network device
 */
static void bnx2_check_link ( struct net_device *netdev ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	struct bnx2_status_block *status_blk = bnx2->status_blk;
	uint32_t status_idx = status_blk->status_idx;
	uint32_t new_link = bnx2->status_blk->status_attn_bits & STATUS_ATTN_BITS_LINK_STATE;
	uint32_t old_link = bnx2->status_blk->status_attn_bits_ack & STATUS_ATTN_BITS_LINK_STATE;
	if ( new_link != old_link ) {
		if ( new_link ) {
			bnx2_set_link ( bnx2 );
			writel ( STATUS_ATTN_BITS_LINK_STATE, bnx2->regs + BNX2_PCICFG_STATUS_BIT_SET_CMD );
			netdev_link_up ( netdev );
		}
		else {
			writel ( STATUS_ATTN_BITS_LINK_STATE, bnx2->regs + BNX2_PCICFG_STATUS_BIT_CLEAR_CMD );
			netdev_link_down ( netdev );
		}
		writel ( BNX2_PCICFG_INT_ACK_CMD_MASK_INT | BNX2_PCICFG_INT_ACK_CMD_INDEX_VALID | status_idx, bnx2->regs + BNX2_PCICFG_INT_ACK_CMD );
		writel ( BNX2_PCICFG_INT_ACK_CMD_INDEX_VALID | status_idx, bnx2->regs + BNX2_PCICFG_INT_ACK_CMD );
		writel ( bnx2->hc_cmd | BNX2_HC_COMMAND_COAL_NOW_WO_INT, bnx2->regs + BNX2_HC_COMMAND );
		readl ( bnx2->regs + BNX2_HC_COMMAND );
	}
}

/******************************************************************************
 *
 * Network device interface
 *
 ******************************************************************************
 */
static void bnx2_refill_rx ( struct bnx2_nic *bnx2 ) {
	struct bnx2_rx_ring_info *rxr = &bnx2->rx_ring;
	struct bnx2_rx_bd *rxbd;
	struct io_buffer *iobuf;
	unsigned int rx_idx;

	while ( ( rxr->prod - rxr->cons ) < BNX2_RX_FILL ) {
		iobuf = alloc_iob ( RX_BUF_SIZE );
		if ( ! iobuf )
			return;

		rxr->prod_bseq += RX_BUF_USE_SIZE;
		rx_idx = ( rxr->prod++ % RX_DESC_CNT );
		if ( ( ( rxr->prod - 1 ) & ( MAX_RX_DESC_CNT - 1 ) ) == ( MAX_RX_DESC_CNT - 1 ) )
			rxr->prod++;

		rxbd = &rxr->desc[rx_idx];

		memset ( iobuf->data, 0, RX_BUF_SIZE );
		rxbd->haddr_lo = virt_to_bus ( iobuf->data );
		wmb();

		bnx2->rx_iobuf[rx_idx] = iobuf;

		writew ( rxr->prod, bnx2->regs + MB_RX_CID_ADDR + BNX2_L2CTX_HOST_BDIDX );
		writel ( rxr->prod_bseq, bnx2->regs + MB_RX_CID_ADDR + BNX2_L2CTX_HOST_BSEQ );
	}
}
/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int bnx2_open ( struct net_device *netdev ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	int rc;

	if ( ( rc = bnx2_init_nic ( bnx2 ) ) )
		return rc;

	//bnx2_check_link ( netdev );

	bnx2_refill_rx ( bnx2 );
	return 0;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
static void bnx2_close ( struct net_device *netdev ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	( void ) bnx2;
}

/**
 * Transmit packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int bnx2_transmit ( struct net_device *netdev,
			       struct io_buffer *iobuf ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	struct bnx2_tx_ring_info *txr = &bnx2->tx_ring;
	struct bnx2_tx_bd *txbd;
	unsigned int tx_idx;
	unsigned int tx_tail;
	if ( ( txr->prod - txr->cons ) >= ( int ) TX_DESC_CNT ) {
		DBGC ( bnx2, "BNX2 %p out of transmit descriptors\n", bnx2 );
		return -ENOBUFS;
	}
	tx_idx = ( txr->prod++ % TX_DESC_CNT );
	if ( ( ( txr->prod - 1 ) & ( MAX_TX_DESC_CNT - 1 ) ) == ( MAX_TX_DESC_CNT - 1 ) )
		txr->prod++;

	tx_tail = ( txr->prod % TX_DESC_CNT );

	txbd = &txr->desc[tx_idx];
	txbd->haddr_lo = virt_to_bus ( iobuf->data );
	txbd->nbytes = iob_len ( iobuf );
	wmb();
	txr->prod_bseq += iob_len ( iobuf );
	writew ( tx_tail, bnx2->regs + MB_TX_CID_ADDR + BNX2_L2CTX_TX_HOST_BIDX );
	writel ( txr->prod_bseq, bnx2->regs + MB_TX_CID_ADDR + BNX2_L2CTX_TX_HOST_BSEQ );
	return 0;
}

static void bnx2_poll_tx ( struct net_device *netdev ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	struct bnx2_tx_ring_info *txr = &bnx2->tx_ring;
	uint16_t hw_cons = bnx2->status_blk->status_tx_quick_consumer_index0;

	while ( txr->cons !=  hw_cons) {
		netdev_tx_complete_next ( netdev );
		txr->cons++;
	}
}

static void bnx2_poll_rx ( struct net_device *netdev ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	struct bnx2_rx_ring_info *rxr = &bnx2->rx_ring;
	struct io_buffer *iobuf;
	unsigned int rx_idx;
	uint32_t length;
	uint16_t hw_cons = bnx2->status_blk->status_rx_quick_consumer_index0;
	struct bnx2_l2_fhdr *hdr;

	rmb();
	if ( ( hw_cons & MAX_RX_DESC_CNT ) == MAX_RX_DESC_CNT )
		hw_cons++;

	while ( rxr->cons != hw_cons ) {
		rx_idx = ( rxr->cons % RX_DESC_CNT );

		iobuf = bnx2->rx_iobuf[rx_idx];
		bnx2->rx_iobuf[rx_idx] = NULL;
		hdr = ( struct bnx2_l2_fhdr * ) iobuf->data;
		iobuf->data += sizeof ( struct bnx2_l2_fhdr ) + 2;
		length = hdr->pkt_len - 4 + sizeof ( struct bnx2_l2_fhdr ) + 2;

		iob_put ( iobuf, length );
		if ( hdr->errors )
			netdev_rx_err ( netdev, iobuf, -EIO );
		else
			netdev_rx ( netdev, iobuf );

		rxr->cons = NEXT_RX_BD ( rxr->cons );
	}
}

/**
 * Poll for completed and received packets
 *
 * @v netdev		Network device
 */
static void bnx2_poll ( struct net_device *netdev ) {
	struct bnx2_nic *bnx2 = netdev->priv;
	bnx2_poll_tx ( netdev );
	bnx2_poll_rx ( netdev );
	bnx2_refill_rx ( bnx2 );
	bnx2_check_link ( netdev );
}

/**
 * Enable or disable interrupts
 *
 * @v netdev		Network device
 * @v enable		Interrupts should be enabled
 */
static void bnx2_irq ( struct net_device *netdev, int enable ) {
	struct bnx2_nic *bnx2 = netdev->priv;

	DBGC ( bnx2, "BNX2 %p does not yet support interrupts\n", bnx2 );
	( void ) enable;
}

/** bnx2 network device operations */
static struct net_device_operations bnx2_operations = {
	.open		= bnx2_open,
	.close		= bnx2_close,
	.transmit	= bnx2_transmit,
	.poll		= bnx2_poll,
	.irq		= bnx2_irq,
};

/******************************************************************************
 *
 * PCI interface
 *
 ******************************************************************************
 */

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @ret rc		Return status code
 */
static int bnx2_probe ( struct pci_device *pci ) {
	struct net_device *netdev;
	struct bnx2_nic *bnx2;
	int rc;

	/* Allocate and initialise net device */
	netdev = alloc_etherdev ( sizeof ( *bnx2 ) );
	if ( ! netdev ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	netdev_init ( netdev, &bnx2_operations );
	bnx2 = netdev->priv;
	pci_set_drvdata ( pci, netdev );
	netdev->dev = &pci->dev;
	memset ( bnx2, 0, sizeof ( *bnx2 ) );
	bnx2->netdev = netdev;
	/* Fix up PCI device */
	adjust_pci_device ( pci );

	bnx2->pci = pci;

	/* Map registers */
	bnx2->regs = ioremap ( pci->membase, BNX2_BAR_SIZE );
	if ( ( rc = bnx2_alloc_mem ( bnx2 ) ) )
		goto err_alloc;

	/* Reset the NIC */
	if ( ( rc = bnx2_reset ( bnx2 ) ) != 0 )
		goto err_reset;

	bnx2_set_mac_address ( bnx2, netdev );
	/* Initialise and reset MII interface */
	mii_init ( &bnx2->mii, &bnx2_mii_operations );
	if ( ( rc = mii_reset ( &bnx2->mii ) ) != 0 ) {
		DBGC ( bnx2, "BNX2 %p could not reset MII: %s\n",
		       bnx2, strerror ( rc ) );
		goto err_mii_reset;
	}
	/* Register network device */
	if ( ( rc = register_netdev ( netdev ) ) != 0 )
		goto err_register_netdev;

	/* Set initial link state */
	bnx2_check_link ( netdev );

	return 0;

	unregister_netdev ( netdev );
 err_register_netdev:
 err_mii_reset:
	bnx2_reset ( bnx2 );
 err_reset:
	netdev_nullify ( netdev );
	netdev_put ( netdev );
 err_alloc:
	return rc;
}

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void bnx2_remove ( struct pci_device *pci ) {
	struct net_device *netdev = pci_get_drvdata ( pci );
	struct bnx2_nic *bnx2 = netdev->priv;

	/* Unregister network device */
	unregister_netdev ( netdev );

	/* Reset card */
	bnx2_reset ( bnx2 );

	bnx2_free_mem ( bnx2 );

	/* Free network device */
	netdev_nullify ( netdev );
	netdev_put ( netdev );
}

/** bnx2 PCI device IDs */
static struct pci_device_id bnx2_nics[] = {
	PCI_ROM ( 0x14e4, 0x164c, "bnx2-5708C", "Broadcom NetXtreme II BCM5708C", 0 ),
	PCI_ROM ( 0x14e4, 0x1639, "bnx2-5709C", "Broadcom NetXtreme II BCM5709C", 0 ),
};

/** bnx2 PCI driver */
struct pci_driver bnx2_driver __pci_driver = {
	.ids = bnx2_nics,
	.id_count = ( sizeof ( bnx2_nics ) / sizeof ( bnx2_nics[0] ) ),
	.probe = bnx2_probe,
	.remove = bnx2_remove,
};
