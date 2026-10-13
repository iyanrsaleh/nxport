# nxport

`nxport` (v1.0.0) adalah utilitas CLI C++ kecil yang menampilkan daftar port TCP yang
sedang **listen**, lengkap dengan proses pemiliknya **dan direktori kerja (working
directory) dari proses tersebut**. Berjalan di **Windows dan Linux**.

Alat ini adalah pengganti yang lebih cepat dan mudah diingat untuk perintah
berikut:

```powershell
# Windows
Get-NetTCPConnection -State Listen |
  Select-Object LocalAddress, LocalPort, State, OwningProcess,
    @{Name="Process";Expression={(Get-Process -Id $_.OwningProcess).ProcessName}}
```

```bash
# Linux
ss -tlnp
```

Bedanya, `nxport` menambahkan satu kolom `Dir`, sehingga kita bisa tahu *dari
folder mana* sebuah port dibuka:

```
LocalAddress  LocalPort  State   PID    Process  Dir
------------  ---------  ------  -----  -------  -------------
0.0.0.0       8099       Listen  14829  python3  /tmp/demosite
```

Kolom `Dir` berisi **direktori kerja (current working directory)** milik proses
pemilik port (di Windows dibaca dari PEB proses tujuan; di Linux dari
`/proc/<pid>/cwd`). Jadi ini adalah folder tempat program dijalankan atau folder
yang dilayani — bukan lokasi file `.exe`/binernya.

## Fitur

- **Dasbor Interaktif Real-time (default)**: Menjalankan `nxport` membuka dasbor
  interaktif yang tetap hidup di terminal — navigasi dengan panah, buka browser,
  kill port, ping, dan refresh, semua langsung dari dasbor.
- **Pencarian Pintar (smart search)**: `nxport <kata>` langsung memfilter
  berdasarkan **nama proses** atau **folder (`Dir`)** — mis. `nxport node`,
  `nxport warkop`.
- **Menu Pengaturan Tersimpan (`s`)**: folder yang **disembunyikan** disimpan
  permanen (tetap diterapkan antar sesi). Port dari folder tersebut tidak
  ditampilkan di daftar utama — kelola daftarnya lewat *Daftar Folder
  Disembunyikan* di dalam dasbor.
- Menampilkan port TCP yang listen beserta PID dan nama prosesnya + kolom `Dir`.
- **Kill Port Langsung**: tombol `[k]` di dasbor, atau `nxport kill <port>`.
- **Buka di Browser**: tombol `[Enter]`/`[o]` di dasbor membuka `http://localhost:<port>`.
- **Health Check Cepat**: tombol `[p]` di dasbor, atau `nxport -p` / `--ping`.
- **Mode Watch (non-interaktif)**: `nxport -w` untuk auto-refresh di pipe/script.
- **Sniffer Tech Stack Otomatis**: `[Next.js]`, `[Express]`, `[FastAPI]`, `[Laravel]`, dll.
- **Deteksi Kontainer**: proses `[Docker]` dan port forwarding `[WSL]`.
- **Audit Keamanan Binding**: peringatan `[PUB!]` jika database terbuka ke `0.0.0.0`.
- **Dukungan UDP**: `nxport -u`.
- **Ekspor Mesin**: `--json`, `--yaml`, `--csv`.
- **Filter Range Port**: `nxport 8000-8090`.
- **Wait & Find Port**: `nxport wait <port>` (blok sampai port up) dan
  `nxport find` (cari port bebas) — cocok untuk script/CI.
- **Watch-diff**: port yang baru muncul/hilang ditandai `*`/`[NEW]`/`[GONE]`.
- **Search & Sort di dasbor**: `/` untuk mencari, `c` untuk ganti urutan.
- **Auto-deteksi perintah start**: mengenali `npm/pnpm/yarn/bun run dev`,
  `python manage.py runserver`, `php artisan serve`, `cargo run`, `go run .`, dll.
- **Penanda warna port**: 🟢 Hijau = unik/sehat, 🔴 Merah = duplikat/konflik.
- **Ekstensi VS Code / NxCode**: lihat & kelola port langsung dari IDE
  (`vscode-nxport/`).
- `LocalAddress` IPv6 `::` dinormalisasi menjadi `0.0.0.0`.
- Tanpa dependensi pihak ketiga (C++ native di Windows & Linux).

## Cara Build

**Windows** — butuh Visual Studio (MSVC) dengan workload
*Desktop development with C++*:

```powershell
./build.bat        # menghasilkan nxport.exe
```

**Linux** — butuh `g++`:

```sh
./build.sh         # menghasilkan nxport
```

## Cara Menggunakan

Cukup jalankan `nxport` — dasbor interaktif akan terbuka dan **tetap hidup**
(tidak langsung keluar) sampai Anda menekan `q`.

```sh
nxport              # dasbor interaktif real-time (default)
nxport 8080         # dasbor, difilter ke port 8080
nxport 8000-8090    # dasbor, difilter rentang port 8000-8090
nxport node         # dasbor, smart search: hanya proses/folder yang memuat "node"
```

**Kontrol dasbor:**

| Tombol | Aksi |
| :--- | :--- |
| `↑` / `↓` (atau `j`/`w`) | Pindah pilih port |
| `Enter` / `o` | Buka `http://localhost:<port>` di browser |
| `k` / `x` | Kill proses pada port terpilih (dengan konfirmasi `y`) |
| `p` | Health check port terpilih |
| `s` | Buka menu **Pengaturan** (hidden folders, Server History, UDP, dll.) |
| `/` | Cari di daftar port (case-insensitive) |
| `c` | Ubah urutan (port/pid/process/addr/dir) |
| `r` | Refresh daftar port |
| `q` / `Esc` | Keluar |

> **Tampilan bebas kedip (flicker-free):** dasbor hanya mengegambar ulang saat
> ada perubahan (tombol ditekan atau data berubah) dan menulis seluruh frame
> dalam satu operasi, dibungkus *synchronized output* terminal. Daftar port
> di-scan ulang tiap 2 detik, namun polling tombol tetap responsif. Baris yang
> tidak muat di tinggi terminal otomatis digeser mengikuti pilihan, sehingga
> layar tidak bergulir/berkedip.

**Mode non-interaktif / untuk script:**

```sh
nxport --once       # cetak tabel sekali lalu keluar (untuk pipe/script)
nxport -w           # watch auto-refresh (pipe/script)
nxport kill 8080    # kill port langsung (dengan konfirmasi)
nxport kill 8080 -f # force kill tanpa konfirmasi
nxport -p           # ping semua port lalu keluar
nxport wait 8080    # tunggu sampai port 8080 aktif (--timeout 60, 0 = selamanya)
nxport find         # cari port bebas pertama dari 3000 (--count 3 / start 8000)
nxport --json       # JSON  (juga: --yaml, --csv)
nxport --all        # semua status TCP, bukan hanya Listen
nxport --no-color   # nonaktifkan warna (juga mematikan dasbor)
nxport --version    # versi (nxport 1.0.0)
nxport --help       # bantuan
```

Contoh keluaran:

```
LocalAddress    LocalPort  State   PID    Process          Dir
--------------  ---------  ------  -----  ---------------  -----------------
0.0.0.0         80         Listen  336    nginx            /
0.0.0.0         443        Listen  336    nginx            /
::              3306       Listen  1079   mysqld           /www/server/data
0.0.0.0         8099       Listen  14829  python3          /tmp/demosite
```

## Menu Pengaturan & Folder Tersembunyi (`s`)

Tekan `s` di dasbor untuk membuka **menu Pengaturan**. Daftar folder yang
**disembunyikan** disimpan permanen ke berkas konfigurasi:

- `%APPDATA%\nxport\settings.conf` (Windows)
- `~/.config/nxport/settings.conf` (Linux)

Item menu:

| Item | Fungsi |
| :--- | :--- |
| `+ Sembunyikan Folder` | Tambah folder untuk **disembunyikan** dari daftar (substring match) |
| `Daftar Folder Disembunyikan` | Lihat & "tampilkan lagi" (hapus dari daftar tersembunyi) |
| `Server History` | Simpan server (folder + perintah) untuk **start/stop** dari dasbor |
| `Tampilkan UDP` | Setara `-u` (toggle `←/→`) |
| `Semua State TCP` | Setara `-a` (toggle `←/→`) |
| `Interval Refresh` | Selang auto-refresh (1–10 detik) |
| `Reset Semua Pengaturan` | Kosongkan semua pengaturan |

Kontrol: `↑`/`↓`, `Enter` pilih/ubah, `←`/`→` toggle, `s`/`Esc` kembali. Di
**Daftar Folder Disembunyikan**: `Enter`/`d` tampilkan lagi, `a` tambah, `Esc`/`s`
kembali.

> Semua proses yang berjalan di folder ini akan **tidak ditampilkan** — baik di
> dasbor maupun di keluaran `--once`/`--json`/`--yaml`/`--csv`.

## Server History (start/stop dari dasbor)

Lewat **`s` → `Server History`** Anda bisa menyimpan server yang sering dipakai
(folder kerja + perintah start) supaya tidak perlu lagi mencari-cari foldernya:

- **Otomatis terisi**: setiap kali `nxport` dijalankan, daftar diisi dari port
  yang sedang listen pada scan pertama (nama = proses, folder = `Dir`, port).
  Folder yang **sedang di-hide** dilewati, dan folder yang **sudah ada tidak
  diduplikasi** — kalau folder itu sudah punya entri, hanya port-nya yang
  dilengkapi.
- `a` — tambah/ubah server (4 langkah: nama → folder → perintah → port opsional).
  Jika foldernya sudah ada di daftar, perintahnya akan diperbarui (tidak dobel).
- `Enter` / `s` — **start** server (perlu perintah; entri hasil auto-detect belum
  punya perintah, tambahkan lewat `a`).
- `x` — **stop** server (matikan lewat port, atau cocokkan folder kerjanya).
- `o` — buka `http://localhost:<port>` di browser (jika port diisi).
- `d` — hapus entri dari daftar.
- `↑`/`↓` pilih, `Esc` kembali.

Contoh perintah start: `npm run dev`, `python manage.py runserver`,
`uvicorn app:app --reload`, `cargo run`, `go run .`.

> Isi entri disimpan di berkas `settings.conf` yang sama. Menjalankan server dari
> sini setara menjalankannya manual di terminal (proses berjalan di background).

## Ekstensi VS Code / NxCode

Folder [`vscode-nxport/`](vscode-nxport/README.md) berisi ekstensi ringan yang
membungkus `nxport --json`:

- View **NxPort Ports** di Explorer (port, proses, alamat, `Dir`).
- Status bar jumlah port aktif; klik untuk membuka daftar.
- **Open in Browser** dan **Stop Port** satu klik (+ Copy URL / Copy Details).
- Auto-refresh berkala; atur path lewat `nxport.command` bila belum di `PATH`.

Menjalankan (development): buka folder `vscode-nxport/` di VS Code → tekan **F5**.
Membuat `.vsix`: `vsce package` lalu `code --install-extension nxport-0.1.0.vsix`.

## Memasang agar Bisa Dipanggil Cukup dengan `nxport`

**Windows** — tambahkan folder ini ke `PATH` **user** (aman, tidak merusak PATH
yang sudah ada), lalu **buka terminal baru**:

```powershell
$dir = 'D:\CLI\NxPort'
$p = [Environment]::GetEnvironmentVariable('Path','User')
if (($p -split ';') -notcontains $dir) {
  [Environment]::SetEnvironmentVariable('Path', ($p.TrimEnd(';') + ';' + $dir), 'User')
}
```

> Jangan pakai `setx PATH "$env:PATH;..."` — perintah itu menggabungkan PATH
> sistem yang panjang, dan `setx` memotong di 1024 karakter sehingga bisa
> merusak `PATH`.
>
> Kalau `nxport` masih belum dikenali di terminal yang sedang terbuka, itu wajar:
> terminal lama belum memuat `PATH` baru. Untuk sesi saat ini saja, jalankan:
>
> ```powershell
> $env:Path += ';D:\CLI\NxPort'
> ```

**Linux** — salin binernya ke folder yang ada di `PATH`:

```sh
sudo install -m755 nxport /usr/local/bin/nxport
```

Setelah itu, di terminal baru cukup jalankan:

```sh
nxport
```

## Catatan

- Baris yang menampilkan `-` berarti prosesnya tidak bisa diperiksa (service
  sistem, milik pengguna lain, atau proses yang dilindungi). Agar lebih banyak
  yang terbaca:
  - **Windows**: jalankan terminal **sebagai administrator**.
  - **Linux**: jalankan dengan `sudo` (membaca `/proc/<pid>/fd` dan
    `/proc/<pid>/cwd` milik pengguna lain memerlukan akses root).
- Build Windows memakai `iphlpapi` + `ws2_32`; build Linux hanya memakai `/proc`.
  Tidak ada dependensi pihak ketiga.

## Integrasi NxCode IDE

Port yang dilayani **NxCode IDE** di dalam proses `editor.exe` biasanya
menampilkan `Dir` = folder instalasi IDE. `nxport` membaca QSettings milik IDE
(`%APPDATA%\NXC-Dev\Editor IDE.ini` → `[server] entries`) dan otomatis
menampilkan folder proyek yang sesuai pada kolom `Dir`:

```
0.0.0.0  8080  Listen  12024  editor.exe  D:/Website/nxdom
0.0.0.0  8081  Listen  12024  editor.exe  D:/Website/Tatiye
```

## Lisensi

MIT — lihat [LICENSE](LICENSE). Riwayat perubahan ada di
[CHANGELOG.md](CHANGELOG.md).
