#!/usr/bin/env python3
"""Pushes a Samsung .hd firmware image into a CLP-620ND from macOS.

A Samsung .hd is sent to the printer verbatim.  The image carries its own UEL
and @PJL header, the bootloader recognises it in the data stream, and that is
the whole protocol - there is no "enter firmware mode" command to send first.
This tool therefore streams the file byte for byte and adds nothing.

On the two things that look like they mean firmware but do not:

  FWV in CMD:SPL,PCL5E,PCL6,FWV,EXT is the firmware *version* key.  Samsung's
  own LiveUpdate client fails with "couldn't find F/W Version, No FWV key in
  Device ID", so it reads a version through it; it is not a download language,
  and @PJL ENTER LANGUAGE=FWV is not a thing.

  FMU=GOOD in @PJL INFO STATUS is the *Font* Management Unit.  The vendor
  utility's FMU code is all FMU_DATA_FONT, LoadFontListFromDevice and
  LoadFormListFromDevice - fonts and macros, not firmware.

Neither is a read path, and there is no other: the formatter has no command
that hands the image back, which is why this is a flasher and not a dumper.

  ./tools/clp620-flash.py fw.hd --usb                  over the USB bulk pipe
  ./tools/clp620-flash.py fw.hd --net 192.168.179.180  over port 9100
  ./tools/clp620-flash.py fw.hd --usb --dry-run        validate and stop

Transports.  --usb hands the image to Apple's own CUPS usb backend, which owns
the printer-class bulk endpoints; nothing here needs libusb or Homebrew.  --net
opens a socket to port 9100.  The printer takes the image identically on both,
so if USB fights you over device claiming, the network path is the same flash.

Safety.  A truncated or mismatched image bricks the formatter, and the only
recovery is reading flash off the board with a programmer.  So, before a byte
moves: the image must carry a UEL header, must name a model that matches the
printer actually connected, and must be plausibly sized.  Any CUPS queue
holding the same device is disabled for the duration and restored afterwards,
because a print job arriving mid-write is exactly the collision that kills the
board.  Nothing is sent without an explicit typed confirmation unless --force.
"""
import argparse, os, re, shutil, socket, subprocess, sys, time

UEL       = b'\x1b%-12345X'
USB_BACKEND = '/usr/libexec/cups/backend/usb'

# A real CLP-620 image is a few megabytes.  Anything far outside this is a
# truncated download, a stray PDF, or the wrong file entirely.
#
# The ceiling is the formatter's flash part itself - 32 MB per the datasheet.
# An image larger than the flash it is written into cannot be valid whatever
# else is true of it, so that is the one bound here that is a fact rather than
# a guess.  (If the datasheet figure turns out to be 32 Mbit, the real ceiling
# is 4 MB and this stays conservative; it never gets less safe.)
MIN_IMAGE = 512 * 1024
MAX_IMAGE = 32 * 1024 * 1024

# Model tokens Samsung uses in this family.  Finding one that is not ours is
# the single most valuable check here: a CLP-670 image will flash onto a 620
# and take the board with it.
#
# The hyphen is optional on purpose, and the comparison normalises it away.
# The service manual names images 'CLP620 VA.BB.CC.DD.hd' with no hyphen while
# the printer's own device ID says MDL:CLP-620 Series with one, so matching the
# two literally would never agree and the check would silently never fire.
MODEL_RE = re.compile(rb'(CLP-?[0-9]{3}|CLX-?[0-9]{4}|ML-?[0-9]{3,4})')


def norm_model(s):
    """CLP620 and CLP-620 are the same printer."""
    return s.replace('-', '').upper() if s else s


def human(n):
    return '%.1f MB' % (n / 1048576.0) if n >= 1048576 else '%s bytes' % format(n, ',')


# --------------------------------------------------------------- device ----

def usb_uri():
    """The usb:// URI of the attached printer, via the backend's own discovery."""
    try:
        out = subprocess.run([USB_BACKEND], capture_output=True, timeout=20,
                             env={**os.environ, 'DEVICE_URI': ''}).stdout
    except (OSError, subprocess.SubprocessError):
        return None, None
    for line in out.decode('utf-8', 'replace').splitlines():
        m = re.match(r'\S+\s+(usb://\S+)\s+"([^"]*)"\s+"([^"]*)"\s+"([^"]*)"', line)
        if m:
            return m.group(1), m.group(4)          # uri, IEEE-1284 device ID
    return None, None


def net_device_id(ip, timeout=6.0):
    """Model and firmware version over PJL, best effort.  '' if it stays quiet."""
    try:
        s = socket.create_connection((ip, 9100), timeout=timeout)
    except OSError:
        return ''
    s.settimeout(timeout)
    try:
        s.sendall(UEL + b'@PJL\r\n@PJL INFO ID\r\n' + UEL)
        buf, t0 = b'', time.time()
        while time.time() - t0 < timeout:
            try:
                d = s.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
            if b'\x0c' in buf:
                break
        return buf.decode('latin-1', 'replace')
    finally:
        s.close()


def printer_model(device_id):
    """Pull a model token out of an IEEE-1284 device ID or a PJL INFO ID reply."""
    if not device_id:
        return None
    m = MODEL_RE.search(device_id.encode('latin-1', 'replace')
                        if isinstance(device_id, str) else device_id)
    return m.group(1).decode() if m else None


# ---------------------------------------------------------------- image ----

# The header a genuine image opens with, e.g.
#   @PJL FIRMWARE = 0011 "V2.00.01.56 Nov-05-2012"
# This, not "ENTER LANGUAGE=FWV", is the real firmware-download command.
FIRMWARE_RE = re.compile(rb'@PJL\s+FIRMWARE\s*=\s*(\d+)\s+"([^"]*)"')


def inspect(path):
    """Read the image and refuse it unless it looks like CLP firmware.

    Returns (payload_bytes, set_of_model_tokens, version_string, notes).

    The model is taken from the FILENAME as well as the content, and in
    practice only from the filename: a real CLP620_V2.00.01.56.hd contains the
    string "CLP620" exactly nowhere in its 22 MB: the payload is compressed
    image data behind a short PJL header, and Samsung carries the model in the
    name alone.  Scanning only the content therefore finds nothing, and a check
    that finds nothing is a check that never fires - which is worse than no
    check, because it reads as if it passed."""
    size = os.path.getsize(path)
    if size < MIN_IMAGE:
        raise SystemExit("refusing: %s is only %s - a real image is megabytes.\n"
                         "This looks truncated or is not firmware at all."
                         % (path, human(size)))
    if size > MAX_IMAGE:
        raise SystemExit("refusing: %s is %s, far larger than any CLP-620 image."
                         % (path, human(size)))

    with open(path, 'rb') as fh:
        data = fh.read()

    notes = []
    head = data[:65536]
    if not data.startswith(UEL):
        raise SystemExit(
            "refusing: %s does not start with the PJL UEL escape.\n"
            "A Samsung .hd carries its own UEL + @PJL header and is flashed verbatim.\n"
            "A file without one is not an image this printer's bootloader will take -\n"
            "it is a truncated download, an inner payload someone unwrapped, or the\n"
            "wrong file.  There is no wrapper this tool could add that would make it\n"
            "safe, so it will not guess one." % path)
    notes.append("carries its own UEL/PJL header; sending verbatim")

    if b'@PJL' not in head:
        raise SystemExit("refusing: no @PJL command found in the first 64 KB of %s."
                         % path)

    version = None
    fw = FIRMWARE_RE.search(head)
    if fw:
        version = fw.group(2).decode('latin-1').strip()
        notes.append('declares @PJL FIRMWARE = %s "%s"'
                     % (fw.group(1).decode(), version))
    else:
        notes.append("no @PJL FIRMWARE header found - unusual for a stock image")

    models = {m.decode() for m in MODEL_RE.findall(head)}
    from_name = {m.decode() for m in MODEL_RE.findall(
        os.path.basename(path).encode('latin-1', 'replace'))}
    if from_name:
        notes.append("model read from the filename: %s"
                     % ', '.join(sorted(from_name)))
    models |= from_name
    return data, models, version, notes


# ------------------------------------------------------------ CUPS queue ----

def queues_on(uri):
    """Queue names pointing at this device URI."""
    try:
        out = subprocess.run(['lpstat', '-v'], capture_output=True,
                             timeout=15).stdout.decode('utf-8', 'replace')
    except (OSError, subprocess.SubprocessError):
        return []
    # "device for NAME: uri", but localised - German CUPS says "Gerät für
    # NAME: uri".  Anchor on the shape, never on the prose: the URI is the
    # last field, and the queue name is the colon-terminated token before it.
    found = []
    for line in out.splitlines():
        m = re.search(r'(\S+):\s+(\S+)\s*$', line)
        if m and m.group(2) == uri:
            found.append(m.group(1))
    return found


def set_queue(name, enable):
    tool = 'cupsenable' if enable else 'cupsdisable'
    if not shutil.which(tool):
        return False
    try:
        return subprocess.run([tool, name], capture_output=True,
                              timeout=20).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


# ------------------------------------------------------------- transport ----

def send_usb(data, uri, chunk):
    """Hand the image to Apple's usb backend on stdin."""
    if not os.access(USB_BACKEND, os.X_OK):
        raise SystemExit("refusing: %s is missing." % USB_BACKEND)
    env = {**os.environ, 'DEVICE_URI': uri}
    argv = [USB_BACKEND, '1', os.environ.get('USER', 'root'), 'firmware', '1', '']
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        pump(p.stdin, data, chunk)
    except BrokenPipeError:
        pass
    try:
        p.stdin.close()
    except OSError:
        pass
    rc = p.wait()
    err = p.stderr.read().decode('utf-8', 'replace')
    for line in err.splitlines():
        if line.startswith(('ERROR', 'STATE')) or 'DEBUG' not in line:
            print("  backend: %s" % line, file=sys.stderr)
    if rc != 0:
        raise SystemExit(
            "the usb backend exited %d - the image may be partly written.\n"
            "Do NOT power the printer off.  Check the panel; if it is sitting in a\n"
            "download state, re-run the same image before doing anything else." % rc)


def send_net(data, ip, chunk):
    s = socket.create_connection((ip, 9100), timeout=30)
    s.settimeout(120)
    try:
        pump(s, data, chunk, is_socket=True)
        s.shutdown(socket.SHUT_WR)
        # Drain whatever the printer says before it reboots; it usually says
        # nothing at all, and a clean close is the signal that it took the image.
        t0 = time.time()
        while time.time() - t0 < 30:
            try:
                if not s.recv(4096):
                    break
            except socket.timeout:
                break
    finally:
        s.close()


def pump(dst, data, chunk, is_socket=False):
    total, done, t0 = len(data), 0, time.time()
    while done < total:
        block = data[done:done + chunk]
        if is_socket:
            dst.sendall(block)
        else:
            dst.write(block)
            dst.flush()
        done += len(block)
        pct = 100.0 * done / total
        sys.stderr.write("\r  sending %6.1f%%  %s / %s" %
                         (pct, human(done), human(total)))
        sys.stderr.flush()
    sys.stderr.write("\r  sent %s in %.1fs%s\n" %
                     (human(total), time.time() - t0, ' ' * 20))


# ------------------------------------------------------------------ main ----

def main():
    ap = argparse.ArgumentParser(
        description="Flash a Samsung .hd firmware image into a CLP-620ND.")
    ap.add_argument('image', help='the .hd firmware file')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--usb', action='store_true', help='flash over USB (default)')
    g.add_argument('--net', metavar='IP', help='flash over port 9100 instead')
    ap.add_argument('--dry-run', action='store_true',
                    help='validate the image and the device, send nothing')
    ap.add_argument('--force', action='store_true',
                    help='skip the typed confirmation (for scripting)')
    ap.add_argument('--skip-model-check', action='store_true',
                    help='flash even if the image names a different model')
    ap.add_argument('--chunk', type=int, default=32768)
    a = ap.parse_args()

    use_usb = not a.net
    print("== image ==")
    data, models, version, notes = inspect(a.image)
    print("  %s" % a.image)
    print("  %s" % human(len(data)))
    for n in notes:
        print("  %s" % n)
    print("  version: %s" % (version or 'unknown'))
    print("  model tokens: %s" % (', '.join(sorted(models)) or 'none found'))

    print("\n== printer ==")
    uri = dev_id = None
    if use_usb:
        uri, dev_id = usb_uri()
        if not uri:
            raise SystemExit("no USB printer found.  Is it connected and awake?\n"
                             "Use --net <ip> to flash over the network instead.")
        print("  %s" % uri)
        print("  %s" % (dev_id or '(no device ID)'))
    else:
        dev_id = net_device_id(a.net)
        if not dev_id.strip():
            raise SystemExit("no PJL response from %s:9100 - is the printer awake?"
                             % a.net)
        print("  %s:9100" % a.net)
        print("  %s" % ' '.join(dev_id.split()))

    found = printer_model(dev_id)
    print("  model: %s" % (found or 'unknown'))

    # The brick-preventing check.  An image for a sibling model will be accepted
    # by the FWV loader and will take the formatter with it.
    if models and found:
        if norm_model(found) not in {norm_model(m) for m in models} \
                and not a.skip_model_check:
            raise SystemExit(
                "\nREFUSING: the image names %s but the printer is a %s.\n"
                "Flashing a sibling model's image bricks the formatter.  If you are\n"
                "certain the image is right, re-run with --skip-model-check."
                % ('/'.join(sorted(models)), found))
        print("  model check: image matches the connected printer")
    elif not a.skip_model_check:
        print("  model check: INCONCLUSIVE - could not read a model from "
              "%s" % ('the image' if not models else 'the printer'))

    held = []
    if use_usb:
        held = queues_on(uri)
        if held:
            print("\n  CUPS queues on this device: %s" % ', '.join(held))
            print("  they will be disabled during the flash and re-enabled after")

    if a.dry_run:
        print("\ndry run: the image and device validate.  Nothing was sent.")
        return 0

    print("\n" + "=" * 68)
    print("  A firmware write that is interrupted BRICKS the printer.")
    print("  The only recovery is reading flash off the formatter board.")
    print("  Do not power off the printer or unplug the cable until it has")
    print("  finished rebooting on its own.  This can take several minutes,")
    print("  and the panel may stay dark for part of it.")
    print("=" * 68)

    if not a.force:
        try:
            if input("\nType FLASH to proceed: ").strip() != 'FLASH':
                print("aborted; nothing was sent.")
                return 1
        except (EOFError, KeyboardInterrupt):
            print("\naborted; nothing was sent.")
            return 1

    for q in held:
        print("  disabling queue %s%s" % (q, '' if set_queue(q, False)
                                          else ' (failed - continuing)'))
    try:
        print("\n== flashing ==")
        if use_usb:
            send_usb(data, uri, a.chunk)
        else:
            send_net(data, a.net, a.chunk)
    finally:
        for q in held:
            set_queue(q, True)
            print("  re-enabled queue %s" % q)

    print("\nImage delivered.  The printer reboots on its own now - leave it alone\n"
          "until the panel returns to ready.  Confirm the new version afterwards:")
    if a.net:
        print("  snmpget -v1 -c public -Ovq %s 1.3.6.1.2.1.1.1.0" % a.net)
    else:
        print("  print a configuration page from the panel, or check the web UI")
    return 0


if __name__ == '__main__':
    sys.exit(main())
