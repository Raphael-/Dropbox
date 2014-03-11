#ifndef _fsserver_h
#define _fsserver_h

int run(char *hostname);
int sync_(int *socket,char **buffer);
int initConnection(int *socket,char *hostname);
int sendFileList(int *socket);
int sendFiles(int *socket,char *req_buffer);
int calcMD5(FILE *file,char *md5);
int recvRequests(int *socket,char **buffer,int *length);
int isComplete(char *buffer,int length);
int checkRecvErrors(int *read_size);
int handleLocks(int *socket,char *curr_file);
int sendLockRequest(int *socket);
int sendMessage(int *socket,char *msg);

#endif