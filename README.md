### 1. Sistemin Genel Mimarisi
Sistem iki ana rolden oluşur:
*   **Node 2 (Gönderici):** Yazılım imajını (`firmware_data.h`) parçalara (64 byte) bölerek gönderir.
*   **Node 1 (Alıcı/Root):** Gelen paketleri doğrular, kalıcı hafızaya (CFS) yazar ve onay (ACK) gönderir.

### 2. Paket Yapısı (`ota_msg`)
Güvenilirliği sağlamak için her paket şu bilgileri taşır:
- **Type:** Mesajın türü (Başlatma, Veri, Bitiş, ACK).
- **Block ID:** Paketin sırasını belirler (kayıp paket tespiti için).
- **Checksum:** Veri bütünlüğünü doğrulamak için kullanılan basit hata kontrol kodu.
- **Data:** 64 byte'lık firmware parçası.

### 3. Güvenilir Aktarım Stratejisi
Kod içerisinde uygulanan güvenilirlik mekanizmaları şunlardır:

#### A. Dur ve Bekle (Stop-and-Wait) Protokolü
Gönderici (Node 2), bir veri bloğu gönderdikten sonra `waiting_for_ack` bayrağını `true` yapar ve beklemeye geçer. Alıcıdan ilgili bloğa ait **ACK** mesajı gelmeden bir sonraki bloğa geçilmez. Eğer ACK gelmezse, zamanlayıcı (etimer) tetiklendiğinde aynı paket yeniden gönderilir (Retransmission).

#### B. Veri Bütünlüğü Kontrolü (Checksum)
Her veri paketi gönderilmeden önce `calculate_checksum` fonksiyonu ile hesaplanır. Alıcı taraf, paketi aldığında kendi hesapladığı checksum ile gelen değeri karşılaştırır:
- **Eşleşirse:** Veri CFS'ye yazılır ve ACK gönderilir.
- **Eşleşmezse:** Paket hatalı kabul edilir, yazılmaz ve ACK gönderilmez (böylece gönderici tekrar gönderir).

#### C. Akış Kontrolü (State Machine)
Aktarım üç aşamalı bir durum makinesi (FSM) ile yönetilir:
1.  **OTA_START:** Alıcıya dosya transferinin başlayacağı ve toplam paket sayısı bildirilir.
2.  **OTA_DATA:** Firmware parçaları sırayla gönderilir.
3.  **OTA_END:** Tüm veri bittiğinde gönderilir. Alıcı bu aşamada dosyanın tamamının CRC/Checksum kontrolünü yapar.

### 4. Veri Saklama ve Nihai Doğrulama
Alıcı (Node 1), **CFS (Coffee File System)** kullanarak gelen verileri `received_fw.bin` dosyasına yazar. 
- Aktarım bittiğinde (`OTA_END`), alıcı diskteki dosyayı baştan sona okur.
- Göndericiden gelen "tüm dosyanın checksum değeri" ile diskteki verinin değeri karşılaştırılır.
- Eğer değerler tutuyorsa **"DOGRULAMA BASARILI"** logu basılır; bu, dosyanın kayıpsız ve hatasız iletildiğini kanıtlar.

### 5. İş Parçacığı (Process) Yönetimi
Contiki-NG'nin protothread yapısı (`PROCESS_THREAD`) sayesinde:
- Ağ olayları ve zamanlayıcılar bloke olmadan çalışır.
- `udp_rx_callback` fonksiyonu asenkron olarak gelen paketleri işlerken, ana döngü (`while(1)`) zaman aşımı ve yeniden gönderim mantığını yönetir.

---
**Özetle:** Bu tasarım; paket numaralandırma, onay mekanizması, hata kontrol kodları ve kalıcı depolama doğrulama adımlarıyla UDP'nin güvensiz yapısı üzerinde **güvenilir bir kanal** oluşturmuştur.
