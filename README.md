# NPIT

A lightweight now-playing display for Linux terminals. NPIT shows MPRIS playback, colorful album art, and optional CAVA visualizer and synced lyrics.

![NPIT showing album art, playback details, and lyrics](assets/screenshot.png)

## Install

Install dependencies for your distribution family:

### Arch-based (Arch, EndeavourOS, CachyOS, Manjaro)

```sh
sudo pacman -S base-devel pkgconf curl json-c libpng libjpeg-turbo openssl cairo pango fontconfig glib2 cava
```

### Debian-based (Debian, Ubuntu, Linux Mint)

```sh
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev libjson-c-dev libpng-dev libjpeg-dev libssl-dev libcairo2-dev libpango1.0-dev libfontconfig1-dev libglib2.0-dev cava
```

### RPM-based (Fedora)

```sh
sudo dnf install gcc make pkgconf-pkg-config libcurl-devel json-c-devel libpng-devel libjpeg-turbo-devel openssl-devel cairo-devel pango-devel fontconfig-devel glib2-devel cava
```

Then build and install:

```sh
git clone https://github.com/buscook/npit.git
cd npit
make
sudo make install
npit
```

MPV also needs `mpv-mpris` to expose playback to NPIT.

## Use

Press `q` to quit. Run `npit --help` for options, or `npit --print-config-path` to find your settings file. NPIT creates that file on first run.

Spotify queue and playlist information are optional. To enable them, create a Spotify Developer app with redirect URI `http://127.0.0.1:8888/callback` and set `SPOTIPY_CLIENT_ID` to its client ID before starting NPIT.

## License

MIT
