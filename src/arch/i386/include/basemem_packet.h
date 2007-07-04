#ifndef BASEMEM_PACKET_H
#define BASEMEM_PACKET_H

#include <realmode.h>

/** Maximum length of base memory packet buffer */
#define BASEMEM_PACKET_LEN 1514

/** Base memory packet buffer */
extern char __data16_array ( basemem_packet, [BASEMEM_PACKET_LEN] );
#define basemem_packet __use_data16 ( basemem_packet )

#endif /* BASEMEM_PACKET_H */