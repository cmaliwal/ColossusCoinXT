#!/bin/bash

set -e

HOSTS="i686-w64-mingw32 x86_64-w64-mingw32"
CONFIGFLAGS="--enable-reduce-exports --disable-bench --disable-gui-tests"
HOST_CFLAGS="-O2 -g"
HOST_CXXFLAGS="-O2 -g"

export QT_RCC_TEST=1
export GZIP="-9n"

cd ../..
# test if we are in the root directory of repository
if [[ ! -e "autogen.sh" ]]
then
    echo "autogen.sh is not found, not root directory of the github repository?"
    exit 1
fi

OUTDIR=`pwd`/outdir
BASEPREFIX=`pwd`/depends
# Build dependencies for each host
for i in $HOSTS; do
  make -C ${BASEPREFIX} HOST="${i}"
done

# Create the release tarball using (arbitrarily) the first host
./autogen.sh
CONFIG_SITE=${BASEPREFIX}/`echo "${HOSTS}" | awk '{print $1;}'`/share/config.site ./configure --prefix=/
make dist
SOURCEDIST=`echo colx-*.tar.gz`
DISTNAME=`echo ${SOURCEDIST} | sed 's/.tar.*//'`

# Correct tar file order
mkdir -p temp
pushd temp
tar xf ../$SOURCEDIST
find colx-* | sort | tar --no-recursion --mode='u+rw,go+r-w,a+X' --owner=0 --group=0 -c -T - | gzip -9n > ../$SOURCEDIST
mkdir -p $OUTDIR/src
cp ../$SOURCEDIST $OUTDIR/src
popd

ORIGPATH="$PATH"
# Extract the release tarball into a dir for each host and build
for i in ${HOSTS}; do
  export PATH=${BASEPREFIX}/${i}/native/bin:${ORIGPATH}
  mkdir -p distsrc-${i}
  cd distsrc-${i}
  INSTALLPATH=`pwd`/installed/${DISTNAME}
  mkdir -p ${INSTALLPATH}
  tar --strip-components=1 -xf ../$SOURCEDIST

  CONFIG_SITE=${BASEPREFIX}/${i}/share/config.site ./configure --prefix=/ --disable-ccache --disable-maintainer-mode --disable-dependency-tracking ${CONFIGFLAGS} CFLAGS="${HOST_CFLAGS}" CXXFLAGS="${HOST_CXXFLAGS}"
  make
  make deploy
  make install DESTDIR=${INSTALLPATH}
  cp -f colx-*setup*.exe $OUTDIR/
  cd installed
  # mv ${DISTNAME}/bin/*.dll ${DISTNAME}/lib/ # temporarily disabled for Zerocoin
  # find . -name "lib*.la" -delete            # temporarily disabled for Zerocoin
  # find . -name "lib*.a" -delete             # temporarily disabled for Zerocoin
  rm -rf ${DISTNAME}/lib/pkgconfig
  find ${DISTNAME}/bin -type f -executable -exec ${i}-objcopy --only-keep-debug {} {}.dbg \; -exec ${i}-strip -s {} \; -exec ${i}-objcopy --add-gnu-debuglink={}.dbg {} \;
  # find ${DISTNAME}/lib -type f -exec ${i}-objcopy --only-keep-debug {} {}.dbg \; -exec ${i}-strip -s {} \; -exec ${i}-objcopy --add-gnu-debuglink={}.dbg {} \;
  find ${DISTNAME} -not -name "*.dbg"  -type f | sort | zip -X@ ${OUTDIR}/${DISTNAME}-${i}.zip
  find ${DISTNAME} -name "*.dbg"  -type f | sort | zip -X@ ${OUTDIR}/${DISTNAME}-${i}-debug.zip
  cd ../../
  rm -rf distsrc-${i}
done

cd $OUTDIR
rename 's/-setup\.exe$/-setup-unsigned.exe/' *-setup.exe
find . -name "*-setup-unsigned.exe" | sort | tar --no-recursion --mode='u+rw,go+r-w,a+X' --owner=0 --group=0 -c -T - | gzip -9n > ${OUTDIR}/${DISTNAME}-win-unsigned.tar.gz
mv ${OUTDIR}/${DISTNAME}-x86_64-*-debug.zip ${OUTDIR}/${DISTNAME}-win64-debug.zip
mv ${OUTDIR}/${DISTNAME}-i686-*-debug.zip ${OUTDIR}/${DISTNAME}-win32-debug.zip
mv ${OUTDIR}/${DISTNAME}-x86_64-*.zip ${OUTDIR}/${DISTNAME}-win64.zip
mv ${OUTDIR}/${DISTNAME}-i686-*.zip ${OUTDIR}/${DISTNAME}-win32.zip

echo 'Done.'
