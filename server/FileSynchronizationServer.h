#ifndef _fsserver_h
#define _fsserver_h

int initConnection(int *server_sck,struct sockaddr_in *client);
int isComplete(char *buffer,int length);
int recvFilesList(char **buffer,int *socket,int *buffer_size);
int manageFilesList(int *socket,char *buffer,int *length);
int shouldSync(char *name,char *md5);
int calcMD5(FILE *file,char *md5);
int recvFiles(int *socket,char *buffer,int *length,int client);
int getData(int *socket,FILE *fptr,int *fsize);
int recvFile(int *socket,char *name,int *fsize,int client);
void *serveClient(void *args);
void *startTimer(void *args);
int recvLockRequest(int *socket);
int checkRecvErrors(int *read_size);
int signalNext();
int sendMessage(int *socket,char *msg,int length);

#endif
