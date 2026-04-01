#define MAX_FILES 4
#define MAX_PACKET_SIZE 4096


typedef struct {
    int packet_type; // = 1
    int file_id;
    char file_name[50];
    int total_chunks;
    int chunk_size;
} file_defn_packet_t;

typedef struct {
    int packet_type;   // = 3
    int file_id;
    int seq_num;
    char filename[50];
} retrans_packet_t;

typedef struct {
    int packet_type; // = 2
    int data_size;
    int file_id;
    int seq_num;
    unsigned int checksum;
    char data[]; // Must dynamically allocate elements.
} data_packet_t; 

typedef struct {
    int packet_type;   // = 4
    int file_id;
} end_packet_t;

typedef struct {
    int packet_type;   // = 5
    int file_id;
} retrans_recvd_packet_t;

typedef struct {
    int total_chunks;
    int received_chunks;
    char **chunks;     // array of chunk pointers
    int *chunk_sizes;  // size of each chunk
    int general_chunk_size; 
    int done;
    char file_name[50];
} file_state_t;