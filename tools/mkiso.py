#!/usr/bin/env python3
"""
このスクリプトはリポジトリのビルド出力（デフォルトは `bin/`）から
UEFIブート可能なISOイメージを作成します。可能であればEFI FATイメージ
を作成してISOの El Torito EFIブートセクションに埋め込みます。

使い方:
  python3 tools/mkiso.py -o LiteCore.iso
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def find_file_candidates(root: Path) -> dict:
    """Look for BOOTX64.EFI and kernel binary in common locations."""
    res = {"bootx64": None, "kernel": None, "fsimg": None}
    # Common locations based on Makefile
    candidates = [
        root / "bin" / "boot" / "BOOTX64.EFI",
        root / "bin" / "BOOTX64.EFI",
        root / "bin" / "boot" / "BOOTx64.EFI",
    ]
    for p in candidates:
        if p.exists():
            res["bootx64"] = p
            break

    kernel_candidates = [
        root / "bin" / "kernel" / "kernel.bin",
        root / "bin" / "kernel.bin",
        root / "bin" / "KERNEL",
    ]
    for p in kernel_candidates:
        if p.exists():
            res["kernel"] = p
            break

    # optional filesystem image produced by make: bin/fs.img or bin/LiteCore.img
    fs_candidates = [root / "bin" / "fs.img", root / "bin" / "LiteCore.img", root / "bin" / "LiteCore.img"]
    for p in fs_candidates:
        if p.exists():
            res["fsimg"] = p
            break

    return res


def check_command(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def run(cmd, **kwargs):
    print("+ ", " ".join(map(str, cmd)))
    subprocess.run(cmd, check=True, **kwargs)


def create_efi_fat_image(efi_img: Path, bootx64: Path, kernel: Path | None, size_mb: int = 32):
    """Create a FAT image and copy EFI and kernel into it.

    Uses mkfs.vfat + mcopy if available (no root required). Returns True on success.
    """
    mkfs = shutil.which("mkfs.vfat") or shutil.which("mkfs.fat")
    mcopy = shutil.which("mcopy")
    if not mkfs or not mcopy:
        return False

    # create sparse file
    with open(efi_img, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

    run([mkfs, "-F", "32", str(efi_img)])

    # use mcopy to copy files into the image. -i selects the image file.
    # ensure target dirs exist using mmd
    mmd = shutil.which("mmd")
    if mmd:
        run([mmd, "-i", str(efi_img), "::/EFI"])  # create EFI
        run([mmd, "-i", str(efi_img), "::/EFI/BOOT"])  # create EFI/BOOT
    else:
        # fallback: mcopy can create dirs with -s when copying full path
        pass

    run([shutil.which("mcopy"), "-i", str(efi_img), str(bootx64), "::/EFI/BOOT/BOOTX64.EFI"]) 
    if kernel:
        run([shutil.which("mcopy"), "-i", str(efi_img), str(kernel), "::/kernel.bin"]) 

    return True


def create_raw_usb_image(img_path: Path, bootx64: Path, kernel: Path | None, fsimg: Path | None = None, size_mb: int = 256):
    """Create a raw USB image file with GPT + one FAT32 ESP partition and copy EFI/kernel into it.

    This operation requires root privileges for losetup/parted/mkfs/mount. The function will use
    'sudo' for privileged commands. The caller must ensure sudo is available and the user has rights.
    """
    img_path = Path(img_path)
    work = tempfile.mkdtemp(prefix="litecore-")
    loop_dev = None
    mount_point = None
    try:
        print("Creating zero-filled image:", img_path, f"({size_mb} MiB)")
        run(["dd", "if=/dev/zero", f"of={str(img_path)}", "bs=1M", f"count={size_mb}"])

        print("Creating GPT and FAT partition (parted)")
        run(["sudo", "parted", "--script", str(img_path), "mklabel", "gpt"]) 
        run(["sudo", "parted", "--script", str(img_path), "mkpart", "ESP", "fat32", "1MiB", "100%"])
        run(["sudo", "parted", "--script", str(img_path), "set", "1", "boot", "on"])

        print("Attaching loop device (with partition scan)")
        loop_dev = subprocess.check_output(["sudo", "losetup", "--partscan", "--find", "--show", str(img_path)], text=True).strip()
        print("loop device:", loop_dev)

        # partition device is usually {loop}p1 (e.g. /dev/loop0p1)
        part_dev = f"{loop_dev}p1"
        # wait for partition device node
        import time
        for _ in range(10):
            if Path(part_dev).exists():
                break
            time.sleep(0.2)
        else:
            raise RuntimeError(f"Partition device {part_dev} not found")

        print("Formatting ESP partition as FAT32:", part_dev)
        run(["sudo", "mkfs.vfat", "-F", "32", part_dev])

        mount_point = Path(tempfile.mkdtemp(dir=work))
        print("Mounting", part_dev, "->", mount_point)
        run(["sudo", "mount", part_dev, str(mount_point)])

        print("Creating EFI directories and copying files")
        run(["sudo", "mkdir", "-p", str(mount_point / "EFI" / "BOOT")])
        run(["sudo", "cp", str(bootx64), str(mount_point / "EFI" / "BOOT" / "BOOTX64.EFI")])
        if kernel:
            run(["sudo", "cp", str(kernel), str(mount_point / "kernel.bin")])
        if fsimg and Path(fsimg).exists():
            run(["sudo", "cp", str(fsimg), str(mount_point / Path(fsimg).name)])

        run(["sudo", "sync"]) 
        run(["sudo", "umount", str(mount_point)])
        mount_point = None

    finally:
        # cleanup
        if mount_point and Path(mount_point).exists():
            try:
                run(["sudo", "umount", str(mount_point)])
            except Exception:
                pass
        if loop_dev:
            try:
                run(["sudo", "losetup", "-d", loop_dev])
            except Exception:
                pass
        if Path(work).exists():
            try:
                shutil.rmtree(work)
            except Exception:
                pass

    return img_path


def build_iso(iso_path: Path, isoroot: Path, efiboot_img: Path | None = None, volume_label: str = "LITECORE"):
    """Create ISO using xorriso/genisoimage/mkisofs.

    Prefer xorriso, fall back to genisoimage/mkisofs. If efiboot_img is provided, embed it as El Torito EFI.
    """
    xorriso = shutil.which("xorriso")
    geniso = shutil.which("genisoimage") or shutil.which("mkisofs")

    # If efiboot_img is provided, xorriso wants that file to be present inside the ISO tree.
    # Copy it into the isoroot (as efiboot.img at ISO root) and reference it by relative path.
    efiboot_iso_path = None
    if efiboot_img and efiboot_img.exists():
        efiboot_iso_path = Path(isoroot) / "efiboot.img"
        shutil.copy2(efiboot_img, efiboot_iso_path)

    if xorriso:
        cmd = [
            "xorriso",
            "-as",
            "mkisofs",
            "-r",
            "-J",
            "-joliet-long",
            "-V",
            volume_label,
            "-o",
            str(iso_path),
        ]
        if efiboot_iso_path and efiboot_iso_path.exists():
            # pass the path inside the ISO (relative to ISO root)
            cmd += ["-eltorito-alt-boot", "-e", str(efiboot_iso_path.name), "-no-emul-boot"]
        cmd.append(str(isoroot))
        run(cmd)
        return

    if geniso:
        cmd = [geniso, "-r", "-J", "-V", volume_label, "-o", str(iso_path)]
        if efiboot_iso_path and efiboot_iso_path.exists():
            cmd += ["-eltorito-alt-boot", "-e", str(efiboot_iso_path.name), "-no-emul-boot"]
        cmd.append(str(isoroot))
        run(cmd)
        return

    raise RuntimeError("No supported ISO creation tool found (xorriso or genisoimage/mkisofs required)")


def copy_tree_into(src: Path, dst: Path):
    for p in src.rglob("*"):
        if p.is_dir():
            continue
        rel = p.relative_to(src)
        target = dst / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, target)


def main(argv=None):
    p = argparse.ArgumentParser(description="Create a bootable ISO for LiteCore")
    p.add_argument("-o", "--output", default="LiteCore.iso", help="output ISO path")
    p.add_argument("--bin-dir", default=".", help="project root (contains bin/) — default: repo root")
    p.add_argument("--no-fsimg", action="store_true",
                   help="do not include bin/fs.img (disable automatic inclusion)")
    p.add_argument("--no-efimg", action="store_true", help="do not create EFI FAT image, always embed EFI file directly in ISO")
    p.add_argument("--workdir", help="temporary workdir (for debugging) — default: system tmp")
    p.add_argument("--usb-img", default="LiteCore.img",
                   help="create a raw USB image (.img) in addition to ISO; path or filename (requires sudo). Default: LiteCore.img")
    p.add_argument("--no-usb-img", action="store_true",
                   help="do not create a raw USB image (disable automatic creation)")
    p.add_argument("--usb-size", type=int, default=512, help="size in MiB for raw USB image (default: 256)")
    args = p.parse_args(argv)

    repo_root = Path(args.bin_dir).resolve()
    out_iso = Path(args.output).resolve()

    found = find_file_candidates(repo_root)
    if not found["bootx64"]:
        print("ERROR: BOOTX64.EFI not found. Run 'make bootloader' first (creates bin/boot/BOOTX64.EFI).", file=sys.stderr)
        sys.exit(1)

    bootx64 = found["bootx64"]
    kernel = found["kernel"]
    fsimg = found["fsimg"]

    workdir = Path(args.workdir) if args.workdir else Path(tempfile.mkdtemp(prefix="litecore-iso-"))
    print("workdir:", workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    isoroot = workdir / "iso_root"
    if isoroot.exists():
        shutil.rmtree(isoroot)
    isoroot.mkdir()

    # Populate EFI dir in isoroot
    efi_dir = isoroot / "EFI" / "BOOT"
    efi_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(bootx64, efi_dir / "BOOTX64.EFI")

    # copy kernel as kernel.bin at ISO root (Makefile placed kernel.bin in ESP root)
    if kernel:
        shutil.copy2(kernel, isoroot / "kernel.bin")

    # copy other useful bin content (bin/lib, bin/apps, README)
    # copy everything under bin/* except object files
    bin_dir = repo_root / "bin"
    if bin_dir.exists():
        for p in bin_dir.iterdir():
            if p.name == ".mnt":
                continue
            # avoid copying .o and internal fs_tmp
            if p.is_dir() and p.name not in ("fs_tmp",):
                copy_tree_into(p, isoroot / p.name)
            elif p.is_file() and not p.name.endswith(".o"):
                shutil.copy2(p, isoroot / p.name)

    # include fs image by default if present (can be disabled with --no-fsimg)
    if (not args.no_fsimg) and fsimg:
        shutil.copy2(fsimg, isoroot / fsimg.name)

    efiboot_img = None
    if not args.no_efimg:
        # try to create a small EFI FAT image
        efiboot_img = workdir / "efiboot.img"
        success = create_efi_fat_image(efiboot_img, bootx64, kernel)
        if not success:
            print("Could not create EFI FAT image automatically (missing mkfs.vfat or mcopy). Will embed EFI file directly into ISO.")
            efiboot_img = None

    try:
        build_iso(out_iso, isoroot, efiboot_img)
    except Exception as e:
        print("ISO creation failed:", e, file=sys.stderr)
        sys.exit(2)

    if not args.no_usb_img and args.usb_img:
        usb_path = Path(args.usb_img)
        if usb_path.exists():
            print("Warning: USB image path already exists, removing and recreating:", usb_path)
            try:
                usb_path.unlink()
            except Exception as e:
                print("Failed to remove existing USB image:", e, file=sys.stderr)
                sys.exit(3)
        try:
            create_raw_usb_image(usb_path, bootx64, kernel, fsimg if (not args.no_fsimg) else None, size_mb=args.usb_size)
            print("Raw USB image created:", usb_path)
        except Exception as e:
            print("Failed to create raw USB image:", e, file=sys.stderr)
            print("Note: creating raw image requires 'sudo' for losetup/parted/mkfs/mount operations.")
            sys.exit(4)

    isohybrid = shutil.which("isohybrid")
    if isohybrid:
        try:
            print("Running isohybrid --uefi on", out_iso)
            run([isohybrid, "--uefi", str(out_iso)])
            print("isohybrid applied; image should be recognized as USB-bootable by tools like Etcher.")
        except subprocess.CalledProcessError as e:
            print("isohybrid failed:", e)
            print("If you need a hybrid (USB raw) image, consider creating a raw disk image with a FAT ESP partition and copying files into it.")
    else:
        print("Note: 'isohybrid' not found. balenaEtcher may complain that the ISO lacks a partition table.")
        print("To make the ISO hybrid (recommended for Etcher), install syslinux and run:")
        print("  isohybrid --uefi ", out_iso)
        print("Alternatively, create a raw USB image (with a GPT/ESP partition) and write that with Etcher.")

    print("ISO created:", out_iso)
    print("You can write the ISO to USB with Rufus or dd. If using Rufus, select 'DD Image' mode for raw write.")
    print("Temporary workdir kept at:", workdir)


# Wow! I've never seen such a crappy Main function! Hey Python! I'm looking at you!
if __name__ == "__main__":
    main()
