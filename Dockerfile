# Copyright (c) 2026 Lars-Erik Jonsson <l@jonsson.es>
# ISC license — see LICENSE for details.

ARG PLATFORM=amd64
FROM ${PLATFORM}/alpine:3.20 AS build

WORKDIR /build

RUN apk add --no-cache \
    wget cmake make gcc g++ linux-headers zlib-dev zlib-static \
    openssl-dev openssl-libs-static \
    automake autoconf libevent-dev ncurses-dev msgpack-c-dev \
    ncurses-static libevent-static msgpack-c ncurses-libs \
    libevent openssl zlib bison libbsd-dev libbsd-static \
    pkgconf git

RUN set -ex; \
    git clone --depth 1 https://github.com/JuliaStrings/utf8proc.git /src/utf8proc; \
    cd /src/utf8proc; \
    make libutf8proc.a; \
    cp libutf8proc.a /usr/lib/; \
    cp utf8proc.h /usr/include/; \
    mkdir -p /usr/lib/pkgconfig; \
    printf 'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: libutf8proc\nDescription: UTF-8 processing library\nVersion: 2.9.0\nLibs: -L${libdir} -lutf8proc\nCflags: -I${includedir}\n' \
      > /usr/lib/pkgconfig/libutf8proc.pc

RUN set -ex; \
    mkdir -p /src/libssh/build; \
    cd /src; \
    wget -O libssh.tar.xz https://www.libssh.org/files/0.10/libssh-0.10.6.tar.xz; \
    tar -xf libssh.tar.xz -C /src/libssh --strip-components=1; \
    cd /src/libssh/build; \
    cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr \
        -DWITH_SFTP=OFF -DWITH_SERVER=ON -DWITH_PCAP=OFF \
        -DWITH_STATIC_LIB=ON -DBUILD_SHARED_LIBS=OFF \
        -DWITH_GSSAPI=OFF ..; \
    make -j $(nproc); \
    make install; \
    # Ensure static lib is installed (cmake may name it ssh_static)
    [ -f /usr/lib/libssh.a ] || cp /src/libssh/build/src/libssh.a /usr/lib/libssh.a 2>/dev/null || \
    cp /src/libssh/build/src/libssh_static.a /usr/lib/libssh.a 2>/dev/null || \
    find /src/libssh/build -name "*.a" -exec cp {} /usr/lib/libssh.a \; ; \
    # Fix pkg-config for static linking: add OpenSSL as private dep
    sed -i 's/^Libs:.*/&\nLibs.private: -lcrypto -lssl -lz/' /usr/lib/pkgconfig/libssh.pc

COPY . .

RUN sh autogen.sh && \
    # Static builds break AC_SEARCH_LIBS checks (can't link test programs).
    # Override the results for functions we know exist on musl/Alpine.
    ./configure --enable-static --enable-utf8proc \
    ac_cv_search_utf8proc_charwidth="-lutf8proc" \
    ac_cv_search_forkpty="none required"
RUN make -j $(nproc)
RUN objcopy --only-keep-debug tmtv tmtv.symbols && chmod -x tmtv.symbols && strip tmtv
RUN ./tmtv -V

FROM alpine:3.20

RUN apk --no-cache add bash
RUN mkdir /build
ENV PATH=/build:$PATH
COPY --from=build /build/tmtv.symbols /build
COPY --from=build /build/tmtv /build
