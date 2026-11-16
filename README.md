# Case_study_3

## Chạy hệ thống

Biên dịch trên Windows bằng MinGW:

```powershell
g++ -std=c++17 RUN_system.cpp -o RUN_system.exe -lws2_32 -lshell32
```

Chạy `RUN_system.exe` từ thư mục dự án. Chương trình sẽ chạy ba thuật toán,
ghi kết quả vào `output/output.txt`, khởi động web server tại cổng 8080 và
tự mở giao diện trên trình duyệt mặc định. Giữ cửa sổ chương trình mở khi sử
dụng web; nhấn `Ctrl+C` để dừng server.

Thư mục `input` và `web` cần nằm cạnh file chạy.