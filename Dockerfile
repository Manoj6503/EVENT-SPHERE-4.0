FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make libsqlite3-dev libssl-dev libcurl4-openssl-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY backend backend
COPY frontend frontend
COPY database database
COPY docs docs
RUN cmake -S backend -B build -DCMAKE_BUILD_TYPE=Release -DES_TESTS=OFF && cmake --build build
# /data is the persistent disk: database, certificates and automatic backups all live there
ENV DB_PATH=/data/eventsphere.db CERT_DIR=/data/certificates BACKUP_DIR=/data/backups PORT=8080 TRUST_PROXY=1
CMD ["sh","-c","mkdir -p /data && ./build/eventsphere"]
