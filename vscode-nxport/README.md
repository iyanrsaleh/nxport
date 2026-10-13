# NxPort — VS Code / NxCode Extension

Ekstensi ringan untuk [VS Code](https://code.visualstudio.com/) (dan IDE turunannya
seperti **NxCode**) yang menampilkan daftar port yang sedang **listen** langsung
di sidebar **Explorer**, dengan tombol satu klik untuk **Open in Browser** dan
**Stop Port**.

Ekstensi ini hanyalah pembungkus tipis: semua deteksi port dilakukan oleh CLI
[`nxport`](../README.md) melalui `nxport --json`.

## Fitur

- **View "NxPort Ports"** di Explorer: setiap port jadi baris dengan nama proses,
  alamat, dan `Dir`.
- **Status bar**: menampilkan jumlah port aktif (mis. `NxPort: 12`), klik untuk
  membuka daftar.
- **Open in Browser**: klik port → buka `http://localhost:<port>`.
- **Stop Port**: konfirmasi lalu hentikan proses (memanggil `nxport kill <port> -f`).
- **Copy URL** dan **Copy Details** lewat menu klik-kanan.
- **Auto-refresh** berkala (bisa dimatikan).
- Port web umum (3000/5173/8080/…) diberi ikon 🌐 hijau, lainnya ikon 📡 biru.

## Prasyarat

`nxport` harus sudah terpasang dan bisa dipanggil dari terminal:

```powershell
nxport --once      # cek sudah jalan
```

Kalau `nxport` belum ada di `PATH`, atur lewat pengaturan `nxport.command`
(mis. `D:\CLI\NxPort\nxport.exe`).

## Menjalankan (mode development)

1. Buka folder `vscode-nxport/` ini di VS Code.
2. Tekan **F5** (Run → Start Debugging) — akan membuka *Extension Development
   Host* dengan ekstensi aktif.
3. Buka panel Explorer → grup **NxPort Ports**.

## Membangun paket (`.vsix`)

```bash
npm i -g @vscode/vsce
vsce package               # menghasilkan nxport-0.1.0.vsix
code --install-extension nxport-0.1.0.vsix
```

## Pengaturan

| Setting | Default | Keterangan |
| :--- | :--- | :--- |
| `nxport.command` | `nxport` | Path ke executable `nxport`. |
| `nxport.autoRefresh` | `true` | Refresh otomatis daftar port. |
| `nxport.refreshInterval` | `5000` | Interval refresh (ms). |
| `nxport.openUrlTemplate` | `http://localhost:{port}` | Template URL untuk Open in Browser. |

## Perintah

| Command | Judul |
| :--- | :--- |
| `nxport.show` | NxPort: Show Listening Ports |
| `nxport.refresh` | NxPort: Refresh Ports |
| `nxport.openPort` | NxPort: Open in Browser |
| `nxport.killPort` | NxPort: Stop Port |
| `nxport.copyUrl` | NxPort: Copy URL |
| `nxport.copyDetails` | NxPort: Copy Port Details |

## Catatan

- Menghentikan proses **sistem** di Windows butuh VS Code dijalankan **as
  Administrator** (sama seperti `nxport kill`).
