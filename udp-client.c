#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

#include "firmware_data.h" 
#include "sys/node-id.h"
#include "sys/log.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_PORT 5678
#define CHUNK_SIZE 64

typedef enum { OTA_START, OTA_DATA, OTA_END, OTA_ACK } ota_type_t;

struct ota_msg {
  uint8_t type;
  uint16_t block_id;
  uint16_t total_blocks;
  uint8_t len;            // Paketteki gerçek veri miktarı
  uint8_t data[CHUNK_SIZE];
  uint16_t block_crc;     // Sadece bu bloğun CRC'si
  uint32_t full_crc;      // Sadece OTA_END tipinde gönderilecek (Tüm dosya CRC32)
  uint16_t checksum;
};

// Basit CRC32 fonksiyonu
uint32_t calculate_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
  }
  return ~crc;
}

static uint16_t current_block = 0;
static bool ota_finished = false;
static bool waiting_for_ack = false;
static uint16_t total_blocks = 0;
static ota_type_t current_state = OTA_START;
static int fd_receiver = -1; 

static struct simple_udp_connection udp_conn;

// Checksum
uint16_t calculate_checksum(const uint8_t *data, size_t len) {
  uint16_t sum = 0;
  for(size_t i = 0; i < len; i++) sum += data[i];
  return sum;
}

/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  struct ota_msg *msg = (struct ota_msg *)data;

  // NODE 2: ACK Alma
  if(node_id == 2 && msg->type == OTA_ACK) {
    if((current_state == OTA_START && msg->block_id == 0xFFFF) || 
       (current_state == OTA_DATA && msg->block_id == current_block)) {
        
        LOG_INFO("ACK Alindi: Blok %u\n", msg->block_id);
        waiting_for_ack = false;

        if(current_state == OTA_START) {
            current_state = OTA_DATA;
        } else {
            current_block++;
        }
    }
    return;
  }

  // NODE 1: Veri Alma ve ACK Gönderme
  if(node_id == 1) {
    struct ota_msg response;
    response.type = OTA_ACK;
    response.block_id = msg->block_id;

    if(msg->type == OTA_START) {
      LOG_INFO("OTA BASLIYOR. Toplam Paket: %u\n", msg->total_blocks);
      cfs_remove("received_fw.bin");
      fd_receiver = cfs_open("received_fw.bin", CFS_WRITE);
      simple_udp_sendto(&udp_conn, &response, sizeof(response), sender_addr);
    } 
    else if(msg->type == OTA_DATA) {
      uint16_t calc = calculate_checksum(msg->data, CHUNK_SIZE);
      if(calc == msg->checksum) {
        if(fd_receiver != -1) {
          cfs_write(fd_receiver, msg->data, CHUNK_SIZE);
          LOG_INFO("Paket %u yazildi. (Checksum OK)\n", msg->block_id);
          simple_udp_sendto(&udp_conn, &response, sizeof(response), sender_addr);
        }
      } else {
        LOG_ERR("Checksum HATASI! Blok %u\n", msg->block_id);
      }
    }
else if(msg->type == OTA_END) {
  if(fd_receiver != -1) {
    cfs_close(fd_receiver);
    fd_receiver = -1;
  }

  LOG_INFO("Aktarim bitti. Butunluk kontrolu yapiliyor...\n");


  int fd_read = cfs_open("received_fw.bin", CFS_READ);
  if(fd_read != -1) {
    uint8_t read_buf[CHUNK_SIZE];
    uint16_t total_checksum = 0;
    int n;
    while((n = cfs_read(fd_read, read_buf, sizeof(read_buf))) > 0) {
      for(int i = 0; i < n; i++) total_checksum += read_buf[i];
    }
    cfs_close(fd_read);

    if(total_checksum == msg->checksum) {
      LOG_INFO("--------------------------------------------\n");
      LOG_INFO("DOGRULAMA BASARILI: Firmware diskte saklandi.\n");
      LOG_INFO("--------------------------------------------\n");
    } else {
      LOG_ERR("DOGRULAMA HATASI: Dosya bozuk! (S:%u vs R:%u)\n", msg->checksum, total_checksum);
    }
  }
  // ---------------------------------------
}
  }
}

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP OTA Process");
AUTOSTART_PROCESSES(&udp_client_process);

PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic_timer;
  uip_ipaddr_t dest_ipaddr;

  PROCESS_BEGIN();


  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

  if(node_id == 1) {
    NETSTACK_ROUTING.root_start();
    LOG_INFO("Node 1 (ROOT) baslatildi.\n");
  }

  etimer_set(&periodic_timer, CLOCK_SECOND * 5);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    if(NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      if(node_id == 2 && !ota_finished) {
        struct ota_msg pkt;
        static int retry_count = 0;

      if(waiting_for_ack) {
        LOG_INFO("ACK gelmedi! Yeniden deneme %d...\n", retry_count);
      } 

        memset(&pkt, 0, sizeof(pkt)); 
        total_blocks = (new_firmware_z1_len + CHUNK_SIZE - 1) / CHUNK_SIZE;

        if(!waiting_for_ack) {
            if(current_state == OTA_START) {
              pkt.type = OTA_START;
              pkt.total_blocks = total_blocks;
              pkt.block_id = 0xFFFF;
              LOG_INFO("OTA START gonderiliyor...\n");
              simple_udp_sendto(&udp_conn, &pkt, sizeof(pkt), &dest_ipaddr);
              waiting_for_ack = true;
            } 
            else if(current_state == OTA_DATA) {
              if(current_block < total_blocks) {
                pkt.type = OTA_DATA;
                pkt.block_id = current_block;
                
                uint32_t offset = (uint32_t)current_block * CHUNK_SIZE;
                uint16_t copy_len = (new_firmware_z1_len - offset >= CHUNK_SIZE) ? CHUNK_SIZE : (new_firmware_z1_len - offset);
                
                memcpy(pkt.data, &new_firmware_z1[offset], copy_len);
                pkt.checksum = calculate_checksum(pkt.data, CHUNK_SIZE);

                LOG_INFO("Paket gonderiliyor: %u/%u\n", current_block + 1, total_blocks);
                simple_udp_sendto(&udp_conn, &pkt, sizeof(pkt), &dest_ipaddr);
                waiting_for_ack = true;
              } else {
                current_state = OTA_END;
              }
            }
            else if(current_state == OTA_END) {
  pkt.type = OTA_END;
  pkt.block_id = 0xFFFE;
  
  // TÜM İMAJIN CHECKSUM'INI HESAPLA (Maddi 7 gereği)
  pkt.checksum = calculate_checksum(new_firmware_z1, new_firmware_z1_len);
  
  LOG_INFO("Tüm paketler bitti. END sinyali gonderiliyor. (Beklenen Checksum: %u)\n", pkt.checksum);
  simple_udp_sendto(&udp_conn, &pkt, sizeof(pkt), &dest_ipaddr);
  ota_finished = true;
}
        } else {
            LOG_INFO("ACK bekliyor... (Yeniden gonderim denenecek)\n");
            waiting_for_ack = false; 
        }
      }
    } else {
      LOG_INFO("Root bekleniyor...\n");
    }

    etimer_set(&periodic_timer, CLOCK_SECOND * 2); 
  }

  PROCESS_END();
}