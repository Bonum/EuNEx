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
    librdkafka1 python3 python3-pip nginx \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install flask

WORKDIR /app

COPY --from=builder /app/build/eunex_me /app/
COPY --from=builder /app/build/test_orderbook /app/
COPY --from=builder /app/build/test_matching_engine /app/
COPY --from=builder /app/build/test_threaded_engine /app/
COPY --from=builder /app/build/test_clearing_house /app/
COPY --from=builder /app/build/test_fix_gateway /app/
COPY --from=builder /app/build/test_ai_trader /app/
COPY --from=builder /app/build/test_stop_orders /app/
COPY --from=builder /app/build/simple_match /app/
COPY --from=builder /app/dashboard/ /app/dashboard/
COPY --from=builder /app/shared/ /app/shared/
COPY --from=builder /app/fix_gateway/ /app/fix_gateway/
COPY --from=builder /app/clearing_house/ /app/clearing_house/
COPY --from=builder /app/run.sh /app/run.sh
COPY --from=builder /app/docker/nginx.conf /etc/nginx/conf.d/default.conf

RUN mkdir -p /app/data /app/logs /app/.pids && chmod +x /app/run.sh

ENV EUNEX_DATA_DIR=/app/data
ENV PYTHONPATH=/app

EXPOSE 7860 8080 8081 9001

CMD ["/app/run.sh"]
