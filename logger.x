/*
 * logger utilizado por server.c para registrar cada operacion hecha
 * por un usuario
 */

const MAX_USER_LEN = 256;
const MAX_OP_LEN = 16;        
const MAX_FILE_LEN = 256;
const MAX_DATE_LEN = 32;

struct log_entry {
    string user_name<MAX_USER_LEN>;   
    string operation<MAX_OP_LEN>;     
    string file_name<MAX_FILE_LEN>;   
    string datetime<MAX_DATE_LEN>;    
};

program LOGGER_PROG {
    version LOGGER_VERS {
        int log_operation(log_entry) = 1;
    } = 1;
} = 0x20000001; /* numero de programa del rango reservado para usuarios (0x20000000-0x3FFFFFFF) */