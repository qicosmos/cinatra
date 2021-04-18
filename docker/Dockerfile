FROM ubuntu:18.04
LABEL maintainer="1072526820@qq.com"
RUN apt-get update -yqq && \
apt-get install -yqq cmake git uuid-dev gcc g++ autoconf
ENV ASIO=/asio
ENV ASIO_INTERNAL=/asio/asio
WORKDIR /
RUN git clone https://github.com/chriskohlhoff/asio.git
WORKDIR $ASIO
RUN git checkout 8087252a0c3c2f0baad96ddbd6554db17a846376
WORKDIR $ASIO_INTERNAL
RUN ./autogen.sh && ./configure
RUN make && make install
ENV CINATRA=/cinatra
WORKDIR $CINATRA
RUN git clone https://github.com.cnpmjs.org/qicosmos/cinatra.git
ENV BOOST=/boost
ENV BOOST_INTERNAL=/boost/boost
WORKDIR $BOOST
RUN git clone --recursive https://github.com/boostorg/boost.git
WORKDIR $BOOST_INTERNAL
RUN apt-get -y install build-essential python-dev autotools-dev libicu-dev libbz2-dev libboost-all-dev
RUN ./bootstrap.sh
RUN ./b2 install
WORKDIR $CINATRA
