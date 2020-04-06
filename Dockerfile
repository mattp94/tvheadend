FROM ubuntu:bionic
WORKDIR app
COPY . .

RUN apt-get update && \
    apt-get install -y tzdata && \
    apt-get install -y git && \
    ./Autobuild.sh -t trusty-amd64 -o deps && \
    ./Autobuild.sh -t trusty-amd64 && \
    apt-get remove -y git && \
    apt-get autoremove -y && \
    apt-get clean

VOLUME /root/.hts/tvheadend
CMD ["./build.linux/tvheadend", "-C"]
