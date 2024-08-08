#export PGSSLMODE=disable

sysbench oltp_read_write \
    --db-driver=pgsql \
    --pgsql-host=localhost \
    --pgsql-port=5433 \
    --pgsql-user=testuser \
    --pgsql-password=testpassword \
    --pgsql-db=testdb \
    --threads=5 \
    --time=60 \
    run
