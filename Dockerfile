FROM alpine:latest AS build

RUN apk update && apk add git cmake build-base zlib-dev
WORKDIR /bloaty
RUN git clone https://github.com/google/bloaty.git
WORKDIR /bloaty/bloaty
RUN cmake -B build -S . && cmake --build build && cmake --build build --target install

FROM alpine:latest

RUN apk update && apk add libstdc++ libgcc

COPY --from=build /bloaty/bloaty/build/bloaty /usr/local/bin/bloaty

# Check that bloaty is installed
RUN bloaty --version

ENTRYPOINT [ "bloaty" ]
