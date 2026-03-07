ARG PLATFORM=amd64
FROM ${PLATFORM}/alpine:3.20 AS build

WORKDIR /build

RUN apk add --no-cache \
    wget cmake make gcc g++ linux-headers zlib-dev openssl-dev \
    automake autoconf libevent-dev ncurses-dev msgpack-c-dev libexecinfo-dev \
    ncurses-static libexecinfo-static libevent-static msgpack-c ncurses-libs \
    libevent libexecinfo openssl zlib bison libbsd-dev \
    pkgconf utf8proc-dev

RUN set -ex; \
    mkdir -p /src/libssh/build; \
    cd /src; \
    wget -O libssh.tar.xz https://www.libssh.org/files/0.10/libssh-0.10.6.tar.xz; \
    tar -xf libssh.tar.xz -C /src/libssh --strip-components=1; \
    cd /src/libssh/build; \
    cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr \
        -DWITH_SFTP=OFF -DWITH_SERVER=OFF -DWITH_PCAP=OFF \
        -DWITH_STATIC_LIB=ON -DWITH_GSSAPI=OFF ..; \
    make -j $(nproc); \
    make install

COPY . .

RUN sh autogen.sh && ./configure --enable-static --enable-utf8proc
RUN make -j $(nproc)
RUN objcopy --only-keep-debug tmtv tmtv.symbols && chmod -x tmtv.symbols && strip tmtv
RUN ./tmtv -V

FROM alpine:3.20

RUN apk --no-cache add bash
RUN mkdir /build
ENV PATH=/build:$PATH
COPY --from=build /build/tmtv.symbols /build
COPY --from=build /build/tmtv /build
