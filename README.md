# NPIT

A lightweight now-playing display for Linux terminals. NPIT shows MPRIS playback or plays a folder of local media, with colorful album art, ASCII video, and optional CAVA visualizer and synced lyrics.

![NPIT showing album art, playback details, and lyrics](assets/screenshot.png)

## Install

Install dependencies for your distribution family:

### Arch-based (Arch, EndeavourOS, CachyOS, Manjaro)

```sh
sudo pacman -S base-devel pkgconf curl json-c libpng libjpeg-turbo openssl cairo pango fontconfig glib2 libvlc vlc-plugins-base ffmpeg cava
```

### Debian-based (Debian, Ubuntu, Linux Mint)

```sh
sudo apt update
sudo apt install build-essential pkg-config libcurl4-openssl-dev libjson-c-dev libpng-dev libjpeg-dev libssl-dev libcairo2-dev libpango1.0-dev libfontconfig1-dev libglib2.0-dev libvlc-dev vlc-plugin-base ffmpeg cava
```

### RPM-based (Fedora)

```sh
sudo dnf install gcc make pkgconf-pkg-config libcurl-devel json-c-devel libpng-devel libjpeg-turbo-devel openssl-devel cairo-devel pango-devel fontconfig-devel glib2-devel vlc-devel vlc ffmpeg cava
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

With `[controls].enabled = true`, press `[` to rewind five seconds and `]` to jump forward five seconds. Arrow keys still change tracks.

Run `npit /path/to/folder` to play audio and video files directly from one folder. NPIT plays them in the order the folder returns them, prefers embedded titles over filenames, and numbers tracks by their position in that order. It does not use filename numbers or media tags to decide playback order. Video appears as moving ASCII in the art area. Folder playback does not require another music player.

For local audio, NPIT shows the embedded artist tag when available. Without one, it repeats the displayed title as the artist.

While NPIT is running, drag an audio file, video file, or folder onto its terminal to play it. Video expands into the available space. With the default `cover` theme, playback text uses one color averaged from samples across the whole video; FFmpeg supplies those samples in the background. Dragged paths work as plain paths, shell-escaped paths, or `file://` URLs.

Player metadata loads in the background, and failed folder drops leave the current song playing. Change `[behavior].metadata_interval` in the settings file to adjust the backup refresh rate.

Spotify queue and playlist information are optional. To enable them, create a Spotify Developer app with redirect URI `http://127.0.0.1:8888/callback` and set `SPOTIPY_CLIENT_ID` to its client ID before starting NPIT.

## License

MIT
