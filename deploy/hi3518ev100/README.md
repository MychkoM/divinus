# Deployment на OpenIPC hi3518ev100 (.142)

Схема переживает reboot камеры (busybox-rc запускает S96divinus на boot).

    # c хоста (нужен /tmp/askpass.sh с паролем камеры):
    gzip -9 -c divinus > /tmp/divinus.gz                     # 512K -> 230K (overlay jffs2 мал!)
    DISPLAY=: SSH_ASKPASS=/tmp/askpass.sh SSH_ASKPASS_REQUIRE=force \
      setsid -w scp -O -o StrictHostKeyChecking=no root@CAM:/tmp/ \
      /tmp/divinus.gz wd.sh S96divinus divinus.yaml
    # на камере:
    mkdir -p /opt/divinus
    gunzip -c /tmp/divinus.gz > /opt/divinus/divinus && chmod +x /opt/divinus/divinus
    mv /tmp/wd.sh /tmp/S96divinus /tmp/divinus.yaml /opt/divinus/ && chmod +x /opt/divinus/{wd.sh,S96divinus}
    ln -sf /opt/divinus/S96divinus /etc/init.d/S96divinus
    rm -f /etc/init.d/S95majestic                            # стоковый majestic выключить
    sh /etc/init.d/S96divinus start

Заметки:
- wd.sh: respawn divinus (~4 с) + ротация лога tail -c 40000 при >150 КБ
  (RAM 28 МБ, /tmp=14 МБ tmpfs; сенсор SC1035 льёт строки в stdout — без ротации RAM-exhaustion).
- S96divinus: запуск ТОЛЬКО через setsid (обычный `( ) &` умирает вместе с exec-каналом dropbear).
- Проверка: netstat :554; /proc/umap/venc Sequence (delta);
  ffmpeg -rtsp_transport tcp RTSP + signalstats UAVG (ночь = 128.0 ровно, день ~147).
