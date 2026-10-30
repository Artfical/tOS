# Faster downloads, and services you can switch on and off

## settings | Services
- New Settings > Services pane: every service a package installed, with a switch. On starts it now and whenever tOS starts, off stops it and keeps it from starting. It asks for your password.
- New service command: service list, status, log, and (with sudo) start, stop, restart, enable, disable.
- Packages bring their own services (tpkg.services) and can print a message when they are installed (tpkg.message). Removing a package stops and removes its services.

## settings | Network
- Downloads are much faster: tOS now accepts up to 4 MB in flight (it was 64 KB), tells the server so with window scaling, receives out-of-order data instead of throwing it away, acknowledges every second segment, and sums packets eight bytes at a time.
- The network card gets as many receive buffers as there is memory for (up to 4096), so a fast server's burst is not dropped.
- A download of any size can be streamed to disk or /dev/null; checked with file contents compared byte for byte, also with packets lost on purpose.

## terminal | Terminal
- A background service's output goes to its own log (service log <name>) and never touches the keyboard.
