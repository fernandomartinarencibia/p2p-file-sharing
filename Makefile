CC = gcc
CFLAGS = -Wall -g -I/usr/include/tirpc
LDFLAGS = -ltirpc

# Archivos fuente generados por rpcgen
RPCGEN_X = logger.x
RPCGEN_GEN = logger_clnt.c logger_svc.c logger_xdr.c logger.h

CUSTOM_SERVER_RPC = rpc_server_changed/logger_server.c

# Objetos
RPC_OBJS = logger_clnt.o logger_xdr.o
LOGGER_OBJS = logger_server.o logger_svc.o logger_xdr.o
SERVER_OBJS = server.o $(RPC_OBJS)

.PHONY: all clean rpcgen

all: rpcgen servidor logger_rpc

# Generar ficheros desde logger.x si no existen
rpcgen:
	@echo "Generando ficheros RPC desde $(RPCGEN_X)..."
	@rpcgen -N -a -M $(RPCGEN_X)
	@rm -f logger_client.c Makefile.logger logger_server.c
	@cp $(CUSTOM_SERVER_RPC) logger_server.c

# Compilación del servidor principal (usa cliente RPC)
servidor: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o servidor $(SERVER_OBJS) $(LDFLAGS)

# Compilación del servidor RPC
logger_rpc: $(LOGGER_OBJS)
	$(CC) $(CFLAGS) -o logger_rpc $(LOGGER_OBJS) $(LDFLAGS)

# Reglas para compilar objetos de código RPC
logger_clnt.o: logger_clnt.c logger.h
	$(CC) $(CFLAGS) -c -o $@ $<

logger_xdr.o: logger_xdr.c logger.h
	$(CC) $(CFLAGS) -c -o $@ $<

logger_svc.o: logger_svc.c logger.h
	$(CC) $(CFLAGS) -c -o $@ $<

logger_server.o: logger_server.c logger.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Regla general
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o servidor logger_rpc $(RPCGEN_GEN) logger_server.c logger_client.c Makefile.logger
