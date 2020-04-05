FROM ubuntu:xenial
WORKDIR app
COPY . .

RUN apt-get update && \
    apt-get install -y git && \
    ./Autobuild.sh -t trusty-amd64 -o deps && \
    ./Autobuild.sh -t trusty-amd64 && \
    apt-get remove -y git && \
    apt-get autoremove -y

VOLUME /root/.hts/tvheadend
CMD ["./build.linux/tvheadend", "-C"]
