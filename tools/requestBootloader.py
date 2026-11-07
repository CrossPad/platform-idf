#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Script for sending bootloader request messages via MIDI SYSEX or USB CDC
Automatically installs required libraries and finds devices
"""

import sys
import subprocess
import time
import argparse

# Espressif device identifiers
ESPRESSIF_VID = 0x303A
ESPRESSIF_NORMAL_PIDS = [0x3456, 0x4001]  # Normal operation PIDs
ESPRESSIF_BOOTLOADER_PIDS = [0x1001, 0x0009]  # Bootloader mode PIDs
# The STM's USB MIDI ("CrossPad MIDI+Serial", 0x0483:5740). It is not the ESP:
# what it receives goes down the STM UART, where the ESP takes a request that
# skips enter_bootloader()'s USB-OTG reset. Never send the ESP request there.
STM_MIDI_PORT_MARK = "midi+serial"

def install_package(package_name, import_name=None):
    """Automatically installs Python package if not available"""
    if import_name is None:
        import_name = package_name

    try:
        __import__(import_name)
        print(f"✓ Package {import_name} is already installed")
        return True
    except ImportError:
        print(f"⚠ Package {import_name} not found. Installing...")
        try:
            subprocess.check_call([
                sys.executable, '-m', 'pip', 'install', package_name
            ])
            print(f"✓ Package {package_name} installed successfully")
            return True
        except subprocess.CalledProcessError as e:
            print(f"✗ Error installing package {package_name}: {e}")
            return False

def ensure_dependencies():
    """Checks and installs all required dependencies"""
    print("Checking dependencies...")

    if not install_package('python-rtmidi', 'rtmidi'):
        print("⚠ Cannot install python-rtmidi. MIDI functionality will be disabled.")
    
    if not install_package('pyserial', 'serial'):
        print("⚠ Cannot install pyserial. USB CDC functionality will be disabled.")
    
    if not install_package('pyusb', 'usb'):
        print("⚠ Cannot install pyusb. USB device detection will be limited.")

    print("✓ Dependency check completed\n")

def find_midi_device(device_name_part):
    """Finds MIDI device by partial name match (case-insensitive)"""
    try:
        import rtmidi

        midiout = rtmidi.MidiOut()
        available_ports = midiout.get_ports()

        print(f"Available MIDI ports:")
        for i, port_name in enumerate(available_ports):
            print(f"  [{i}] {port_name}")

        device_name_part_lower = device_name_part.lower()
        for i, port_name in enumerate(available_ports):
            if STM_MIDI_PORT_MARK in port_name.lower():
                continue
            if device_name_part_lower in port_name.lower():
                print(f"\n✓ Found device: [{i}] {port_name}")
                return i, port_name

        print(f"\n✗ Device containing '{device_name_part}' not found")
        return None, None

    except Exception as e:
        print(f"✗ Error searching for MIDI devices: {e}")
        return None, None

def find_usb_cdc_device(device_name_part, vid=None, pid=None):
    """Finds USB CDC device by partial name match or VID/PID, with auto-detection of Espressif devices"""
    try:
        import serial.tools.list_ports

        available_ports = serial.tools.list_ports.comports()

        print(f"Available USB CDC ports:")
        espressif_devices = []
        for port in available_ports:
            vid_pid_info = ""
            if hasattr(port, 'vid') and hasattr(port, 'pid') and port.vid and port.pid:
                vid_pid_info = f" [VID:PID = {port.vid:04X}:{port.pid:04X}]"
                # Collect Espressif devices (VID = 0x303A)
                if port.vid == ESPRESSIF_VID:
                    espressif_devices.append(port)
            print(f"  {port.device} - {port.description}{vid_pid_info}")

        # First try specific VID/PID if provided
        if vid is not None and pid is not None:
            print(f"Searching by VID:PID = {vid:04X}:{pid:04X}")
            for port in available_ports:
                if hasattr(port, 'vid') and hasattr(port, 'pid'):
                    if port.vid == vid and port.pid == pid:
                        print(f"\n✓ Found device by VID/PID: {port.device} - {port.description}")
                        return port.device, port.description
            print(f"No device found with VID:PID {vid:04X}:{pid:04X}")
            
        # Auto-select Espressif devices if no specific VID/PID provided
        if vid is None and pid is None and espressif_devices:
            print(f"\nAuto-detecting Espressif devices...")
            for port in espressif_devices:
                print(f"✓ Auto-selected Espressif device: {port.device} - {port.description} [VID:PID = {port.vid:04X}:{port.pid:04X}]")
                return port.device, port.description
            
        # Fallback to name matching
        device_name_part_lower = device_name_part.lower()
        for port in available_ports:
            description = port.description.lower()
            print(f"Checking port {port.device}: {port.description}")
            if device_name_part_lower in description:
                print(f"\n✓ Found device: {port.device} - {port.description}")
                return port.device, port.description

        print(f"\n✗ USB CDC device containing '{device_name_part}' not found")
        if not espressif_devices:
            print("✗ No Espressif devices (VID=303A) found for auto-selection")
        return None, None

    except Exception as e:
        print(f"✗ Error searching for USB CDC devices: {e}")
        return None, None

def find_bootloader_device():
    """Check if device is in bootloader mode and find its serial port"""
    try:
        import serial.tools.list_ports
        
        available_ports = serial.tools.list_ports.comports()
        bootloader_ports = []
        
        for port in available_ports:
            if hasattr(port, 'vid') and hasattr(port, 'pid'):
                if port.vid == ESPRESSIF_VID and port.pid in ESPRESSIF_BOOTLOADER_PIDS:
                    bootloader_ports.append(port)
        
        if bootloader_ports:
            print(f"✓ Found {len(bootloader_ports)} Espressif bootloader device(s):")
            for i, port in enumerate(bootloader_ports):
                print(f"  [{i}] {port.device} - {port.description} [VID:PID = {port.vid:04X}:{port.pid:04X}]")
            return bootloader_ports[0].device, bootloader_ports[0].description
        else:
            return None, None
            
    except Exception as e:
        print(f"✗ Error checking for bootloader devices: {e}")
        return None, None

def send_usb_cdc_message(device_port, message_data):
    """Sends message via USB CDC (Serial)"""
    try:
        import serial
        print(f"Attempting to open USB CDC port: {device_port}")
        
        ser = serial.Serial(device_port, 115200, timeout=2)
        time.sleep(0.1)
        
        print(f"Sending USB CDC message: {message_data}")
        ser.write(message_data.encode('utf-8') + b'\n')

        # The firmware resets its USB controller on this request, so the port
        # going away right after the write is the request working, not failing
        # (Linux: EIO from the drain or TIOCINQ; Windows: a SerialException).
        try:
            ser.flush()
            time.sleep(0.1)
            if ser.in_waiting > 0:
                response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                print(f"Device response: {response.strip()}")
        except Exception as e:
            print(f"✓ Device left USB right after the request ({e}) — restarting into the bootloader")
            return True

        print("✓ USB CDC message sent successfully")
        ser.close()
        return True

    except Exception as e:
        print(f"✗ Error sending USB CDC message: {e}")
        return False

def send_sysex_message(device_port, sysex_data):
    """Sends SYSEX message to specified MIDI device"""
    try:
        import rtmidi
        midiout = rtmidi.MidiOut()

        available_ports = midiout.get_ports()
        if device_port >= len(available_ports):
            print(f"✗ Port {device_port} doesn't exist. Available ports: {len(available_ports)}")
            return False

        print(f"Attempting to open port {device_port}: {available_ports[device_port]}")
        midiout.open_port(device_port)

        full_sysex = [0xF0] + sysex_data + [0xF7]
        print(f"Sending SYSEX message: {[hex(b) for b in full_sysex]}")
        midiout.send_message(full_sysex)
        print("✓ SYSEX message sent successfully")

        time.sleep(0.1)
        midiout.close_port()
        del midiout
        return True

    except Exception as e:
        print(f"✗ Error sending SYSEX message: {e}")
        return False

def try_bootloader_request(device_name, method_order, vid=None, pid=None):
    """Try to send bootloader request using specified method order"""
    
    SYSEX_DATA = [0x7D, 0x19, 0x00]  # Manufacturer ID, Bootloader id, ESP target
    CDC_MESSAGE = "BOOTLOADER_REQUEST"
    
    for method in method_order:
        print(f"\n--- Trying {method.upper()} method ---")
        
        if method == "midi":
            device_port, device_desc = find_midi_device(device_name)
            if device_port is not None:
                print(f"Sending SYSEX message to device: {device_desc}")
                if send_sysex_message(device_port, SYSEX_DATA):
                    return True
            else:
                print("MIDI device not found, trying next method...")
                
        elif method == "cdc":
            device_port, device_desc = find_usb_cdc_device(device_name, vid, pid)
            if device_port is not None:
                print(f"Sending CDC message to device: {device_desc}")
                if send_usb_cdc_message(device_port, CDC_MESSAGE):
                    return True
            else:
                print("USB CDC device not found, trying next method...")

    return False

def main():
    """Main function"""
    parser = argparse.ArgumentParser(
        description='Send bootloader request via MIDI SYSEX or USB CDC',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python requestBootloader.py --method midi,cdc    # Try MIDI first, then CDC
  python requestBootloader.py --method cdc,midi    # Try CDC first, then MIDI
  python requestBootloader.py --method midi        # Try MIDI only
  python requestBootloader.py --method cdc         # Try CDC only
  python requestBootloader.py --method cdc --vid 0x303A --pid 0x1001  # Use VID/PID for CDC
        """
    )
    
    parser.add_argument(
        '--method', 
        default='midi,cdc',
        help='Communication method order (comma-separated): midi, cdc (default: midi,cdc)'
    )
    
    parser.add_argument(
        '--device',
        default='Crosspad',
        help='Part of device name to search for (default: Crosspad)'
    )
    
    parser.add_argument(
        '--vid',
        type=lambda x: int(x, 16),
        help='USB Vendor ID in hex format (e.g., 0x303A)'
    )
    
    parser.add_argument(
        '--pid',
        type=lambda x: int(x, 16),
        help='USB Product ID in hex format (e.g., 0x1001)'
    )
    
    parser.add_argument(
        '--skip-bootloader-check',
        action='store_true',
        help='Skip checking if device is already in bootloader mode'
    )
    
    args = parser.parse_args()
    
    print("=== MIDI SYSEX/USB CDC Bootloader Request ===\n")

    # Check and install dependencies
    ensure_dependencies()

    # First, check if device is already in bootloader mode
    if not args.skip_bootloader_check:
        print("=== Checking if device is already in bootloader mode ===")
        bootloader_port, bootloader_desc = find_bootloader_device()
        if bootloader_port:
            print(f"\n✓ Device is already in Espressif bootloader mode!")
            print(f"✓ Bootloader available on: {bootloader_port} ({bootloader_desc})")
            print("\n=== Completed ===")
            return
        else:
            print("Device not in bootloader mode, proceeding with bootloader request...\n")

    # Parse method order
    method_order = [method.strip().lower() for method in args.method.split(',')]
    valid_methods = ['midi', 'cdc']
    
    for method in method_order:
        if method not in valid_methods:
            print(f"✗ Invalid method: {method}. Valid methods: {', '.join(valid_methods)}")
            sys.exit(1)
    
    print(f"Method order: {' -> '.join(method_order)}")
    print(f"Target device: {args.device}")
    if args.vid and args.pid:
        print(f"Target VID:PID: {args.vid:04X}:{args.pid:04X}")

    # Try bootloader request with specified method order
    success = try_bootloader_request(args.device, method_order, args.vid, args.pid)

    if success:
        print("\n✓ Bootloader request sent successfully!")
        print("Wait a moment for device to enter bootloader mode...")
        
        # Wait for device to disconnect and ROM bootloader to re-enumerate USB
        print("\nWaiting for device to reset...")
        time.sleep(3)

        # Poll for bootloader mode
        timeout_sec = 30
        poll_interval = 0.5
        deadline = time.time() + timeout_sec
        entered = False
        attempt = 1

        print(f"Scanning for bootloader device (up to {timeout_sec}s)...")
        while time.time() < deadline:
            bootloader_port, bootloader_desc = find_bootloader_device()
            if bootloader_port:
                print(f"✓ Device entered bootloader mode on: {bootloader_port}")
                entered = True
                break
            time.sleep(poll_interval)
            attempt += 1
            if attempt % 4 == 0:
                print(f"  Still scanning... ({int(deadline - time.time())}s remaining)")

        if not entered:
            print(f"⚠ Device did not enter bootloader mode within {timeout_sec} seconds")
    else:
        print("\n✗ Failed to send bootloader request via any method")
        print("Check if:")
        print("1. Device is connected")
        print("2. Device drivers are installed")
        print("3. Device name is correct")
        print("4. No other applications are using the device")
        print("\n ESP32 or others without USB wont work with this script")
        print("If thats the case comment out \"idf.preFlashTask\": \"python requestBootloader.py\"")
        print("In .vscode/settings.json")
        sys.exit(0)

    print("\n=== Completed ===")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n✗ Interrupted by user (Ctrl+C)")
        sys.exit(0)
    except Exception as e:
        print(f"\n✗ Unexpected error: {e}")
        sys.exit(1)
