#ifndef CLIENT_H
#define CLIENT_H

enum ScannerState {
    STATE_INITIAL,
    STATE_SINGLE,
    STATE_DOUBLE,
    STATE_COMMENT
};
extern enum ScannerState scanner_state;
extern char sql_buffer[10240];
extern int sql_pos;
extern void send_to_server();
extern void reset_sql_buffer();

#endif // CLIENT_H