Building the eCos binary can help us to identify library functions and drastically save us time and effort.

Here, I will briefly explain how to build `CG3100L_V5.5.4_EU_src.tar.bz2` to produce `.o` files.

## Setup the environment
Building this eCos binary is challenging because it relies on ancient software...
[A guide from ecos.wtf](https://ecos.wtf/2021/03/12/ecos-firmware-analysis-with-ghidra) reported a successful compilation on CentOS6.
* The compilation must be done on an x86 CentOS 6, **The toolchain is too old to run on x64**.
* Hyper-V does not like x86 CentOS 6.
* I installed x86 CentOS 6 on a PvE 8.1.4 VM with 4 CPU cores and 4G of RAM.

1. Download [CentOS6 x86 ISO](https://vault.centos.org/6.10/isos/i386/CentOS-6.10-i386-minimal.iso).
2. Install it to a virtual machine.
3. Start the VM.
4. You may need to manually bring up the network interface with `ifup eth0`.
5. You can now ssh into the VM. However, the OpenSSH 5.3 on CentOS6 is very old. To login from a recent OpenSSH client (9.x), add `-o HostKeyAlgorithms=+ssh-dss` to your ssh command line.
6. Update yum repo by saving the following as `fix_centos6.sh` and run it:
```sh
#!/bin/sh

cat <<-'EOF' > /etc/yum.repos.d/CentOS-Base.repo
[C6.10-base]
name=CentOS-6.10 - Base
baseurl=http://vault.epel.cloud/6.10/os/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-6
enabled=1
metadata_expire=never

[C6.10-updates]
name=CentOS-6.10 - Updates
baseurl=http://vault.epel.cloud/6.10/updates/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-6
enabled=1
metadata_expire=never

[C6.10-extras]
name=CentOS-6.10 - Extras
baseurl=http://vault.epel.cloud/6.10/extras/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-6
enabled=1
metadata_expire=never

[C6.10-contrib]
name=CentOS-6.10 - Contrib
baseurl=http://vault.epel.cloud/6.10/contrib/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-6
enabled=0
metadata_expire=never

[C6.10-centosplus]
name=CentOS-6.10 - CentOSPlus
baseurl=http://vault.epel.cloud/6.10/centosplus/$basearch/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-6
enabled=0
metadata_expire=never
EOF

yum -y update
yum -y install sudo wget screen
yum -y groupinstall "Development Tools"
```

## Build the toolchain
Save the following as `toolchain.sh` and run it:
```sh
#!/bin/sh

TARGET="mipsisa32-elf"
PREFIX="${HOME}/gnutools"

mkdir -p ${PREFIX}
mkdir -p /tmp/src

echo "[+] Installing dependencies."
sudo yum install -q -y compat-gcc-34 binutils glibc-devel.i686 tcl unzip

echo "[+] Downloading sources."
wget ftp://ftp.gnu.org/gnu/gcc/gcc-3.2.1/gcc-core-3.2.1.tar.gz
wget ftp://ftp.gnu.org/gnu/gcc/gcc-3.2.1/gcc-g++-3.2.1.tar.gz
wget ftp://ftp.gnu.org/gnu/binutils/binutils-2.13.1.tar.bz2
wget ftp://ftp-stud.fht-esslingen.de/pub/Mirrors/sourceware.org/newlib/newlib-1.11.0.tar.gz
wget https://ftp.gnu.org/gnu/gcc/gcc-3.4.6/gcc-3.4.6.tar.gz

echo "[+] Downloading patches."
wget https://ecos.sourceware.org/binutils-2.13.1-v850-hashtable.patch -O /tmp/src/binutils-2.13.1-v850-hashtable.patch
wget https://ecos.sourceware.org/gcc-3.2.1-arm-multilib.patch -O /tmp/src/gcc-3.2.1-arm-multilib.patch

tar xzf gcc-core-3.2.1.tar.gz -C /tmp/src
tar xzf gcc-g++-3.2.1.tar.gz -C /tmp/src
tar xzf gcc-3.4.6.tar.gz -C /tmp/src
tar xf binutils-2.13.1.tar.bz2 -C /tmp/src
tar xzf newlib-1.11.0.tar.gz -C /tmp/src

cd /tmp/src

echo "[+] Applying patches."
patch -p0 < binutils-2.13.1-v850-hashtable.patch
patch -p0 < gcc-3.2.1-arm-multilib.patch

echo "[+] Moving newlibs."
mv newlib-1.11.0/newlib gcc-3.2.1 > /dev/null 2>&1
mv newlib-1.11.0/libgloss gcc-3.2.1 > /dev/null 2>&1

echo "[+] Building binutils 2.13.1."
mkdir -p /tmp/build/binutils
cd /tmp/build/binutils
/tmp/src/binutils-2.13.1/configure --target=${TARGET} --prefix=${PREFIX}
make -w all
make install

echo "[+] Building GCC 3.4.6"
mkdir -p /tmp/build/gcc-3.4.6
cd /tmp/build/gcc-3.4.6
/tmp/src/gcc-3.4.6/configure --prefix=${PREFIX} --enable-languages=c,c++ --with-gnu-as --with-gnu-ld
make -w all
make install

export PATH="$PATH:${PREFIX}/bin"
export gcc="${PREFIX}/bin/gcc"
export CC="${PREFIX}/bin/gcc"

echo "[+] Building GCC 3.2.1."
mkdir -p /tmp/build/gcc
cd /tmp/build/gcc
/tmp/src/gcc-3.2.1/configure --target=${TARGET} --prefix=${PREFIX} --enable-languages=c,c++ --with-gnu-as --with-gnu-ld --with-newlib --with-gxx-include-dir="${PREFIX}/${TARGET}/include" -v
make -w all
make install
echo "[+] Cleaning up."
cd
rm -f *gz
rm -f *bz2
rm -rf /tmp/src
rm -rf /tmp/build
echo "[+] Done"
```

## Set the correct GCC version
```sh
yum -y remove gcc
export PATH="$PATH:${HOME}/gnutools/bin"
```

## Obtain the source code
```sh
wget https://www.downloads.netgear.com/files/GPL/CG3100L_V5.5.4_EU_V1.0.4_Linux_src.zip
unzip CG3100L_V5.5.4_EU_V1.0.4_Linux_src.zip
cd CG3100L_V5.5.4_EU_V1.0.4_Linux_src
tar xjf CG3100L_V5.5.4_EU_src.tar.bz2
cd rbb_cm_ecos/ecos-src
```

or download this repo as a zip file.

## Patch the eCos script as suggested by ecos.wtf
```sh
# fix tail call syntax
find -name "build.bash" -print -exec sed -i "s/make -s clean/find -iname \"make*\" -exec sed -i \'s\/tail \+2\/tail -n\+2\/g\' \{\} \\\;\nmake -s clean/" {} \;
# remove unnecessary call to diff
find -name "build.bash" -print -exec sed -i 's/^diff.*$//' {} \;
```

## Build eCos
```sh
cd bcm33xx
bash build.bash
```
You may want to manually edit `build.bash` and replace `make -s xxx` with `make` to see a more verbose log.

Sometimes the build does not work out of the box, run `rm bcm33xx_build/hal/mips/bcm33xx/v2_0/plf_defs.inc.deps` and then retry `bash build.bash`.

## Collect binaries
Run these commands on the VM:
```sh
mkdir ~/ecos/libs
find -name "*.o" -print -exec cp {} ~/ecos/libs/ \;
```
Then transfer the `.o` files in `~/ecos/libs/` to your local PC.

## Generate sig file for IDA
Before you start this step, obtain the FLAIR toolchain (which can be downloaded from the IDA portal). Then run:
```cmd
pelf.exe *.o ecos5.5.4.pat
sigmake.exe ecos5.5.4.pat ecos5.5.4.sig
```

`sigmake.exe` will prompt you for collisions. Remove the first four lines of `ecos5.5.4.exc` and rerun `sigmake.exe ecos5.5.4.pat ecos5.5.4.sig`.

Now you should have an `ecos5.5.4.sig` file which is ready to be imported to IDA.

A copy of `ecos5.5.4.sig` can be downloaded [here](ecos5.5.4.sig).

## References
1. https://ecos.wtf/2021/03/12/ecos-firmware-analysis-with-ghidra
2. https://www.youtube.com/watch?v=TqIUsLQ-HjE
3. https://tyeyeah.github.io/2023/01/02/2023-01-02-Binary-SCA-Software-Composition-Analysis-Related-Tools/
4. https://www.boozallen.com/insights/cyber/tech/ida-flirt-signatures-for-linux-binaries.html
5. https://cloud.tencent.com/developer/article/1635289
6. https://nsfocusglobal.com/function-identification-in-reverse-engineering-of-iot-devices/