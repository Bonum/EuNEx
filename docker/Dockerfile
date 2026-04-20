FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake g++ \
    librdkafka-dev \
    python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install flask

WORKDIR /app
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DEUNEX_USE_KAFKA=ON \
             -DEUNEX_BUILD_TESTS=ON \
             -DEUNEX_BUILD_EXAMPLES=ON && \
    cmake --build . --config Release -j$(nproc)

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    librdkafka1 python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install flask

WORKDIR /app

COPY --from=builder /app/build/eunex_me /app/
COPY --from=builder /app/build/test_orderbook /app/
COPY --from=builder /app/build/test_matching_engine /app/
COPY --from=builder /app/build/test_threaded_engine /app/
COPY --from=builder /app/build/simple_match /app/
COPY --from=builder /app/dashboard/ /app/dashboard/

EXPOSE 8080

CMD ["./eunex_me"]
