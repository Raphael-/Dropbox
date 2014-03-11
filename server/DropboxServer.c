#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/md5.h>
#include <pthread.h>
#include "FileSynchronizationServer.h"
#include "queue.h"

#define SERVER_PORT 12010
#define BUFFER_SIZE 512  //set initial buffer size to 512 bytes
#define MAX_CLIENTS 5
#define TIME_TO_UNLOCK 10    //server will wait 10 seconds until the client unlocks the file.

struct pthread_data //this struct will be passed as an argument for each client thread
{
    int socket; //socket descriptor to read/write from
    int client_ID;  //unique client ID
};

struct pthread_timer    //this struct will be passed as an argument for each timer thread
{
    pthread_t to_cancel;
    int socket;
    int fdes;
};

pthread_mutex_t lock1,lock2;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
struct queue Q; //queue of pthreads
int running_threads = 0;
pthread_t flag = 0;  //flag used to wake up a specific thread 

int main(int argc, char ** argv)
{
    int server_sck,client_sck;  //socket descriptors for server and client respectively
    struct sockaddr_in client;
    initConnection(&server_sck,&client);
    int c = sizeof(struct sockaddr_in);
    chdir("./dropbox/"); //change current directory to dropbox folder
    unsigned int client_ID = 1;
    pthread_mutex_init(&lock1,NULL);
    pthread_mutex_init(&lock2,NULL);
    pthread_t threads[MAX_CLIENTS];
    init_queue(&Q);
    int i = 0;
    while(1)
    {
        for(i = 0;i < MAX_CLIENTS;i++)
        {
            printf("Waiting for client to connect\n");
            client_sck = accept(server_sck, (struct sockaddr *)&client, (socklen_t*)&c);  //accept connection from a client
            if(client_sck < 0)
            {
                fprintf(stderr,"Accept failed.Retrying..\n");
                i--;    //do not increase i
                continue;
            }
            printf("Connection accepted\n");
            pthread_mutex_lock(&lock1);
            running_threads++;
            pthread_mutex_unlock(&lock1);
            struct pthread_data *data;
            data = malloc(sizeof(struct pthread_data));
            data->socket = client_sck;
            data->client_ID = client_ID++;
            pthread_create(&threads[i],NULL,serveClient,data);
        }
        if(running_threads != 0)
        {
            for(i = 0;i < MAX_CLIENTS;i++)
                pthread_join(threads[i],NULL);
        }
    }
    pthread_mutex_destroy(&lock1);
    pthread_mutex_destroy(&lock2);
    pthread_cond_destroy(&cond);
    return 0;
}

void *serveClient(void *args)
{
    int buffer_size = BUFFER_SIZE;
    struct pthread_data *data = args;
    char *client_message = malloc(buffer_size); //a buffer where client's messages will be stored
    if(client_message == NULL)
    {
        fprintf(stderr,"malloc failed.Server will close this connection.\n");
        close(data->socket);
        pthread_mutex_lock(&lock1);
        running_threads--;
        pthread_mutex_unlock(&lock1);
        free(args);
        return NULL;
    }
    memset(client_message, 0, buffer_size);  //clears the buffer
    int rcvBytes = recvFilesList(&client_message,&data->socket,&buffer_size);
    if(rcvBytes) //if receive files was successful
    {
        printf("Successfully received files list from client %d\n",data->client_ID);
        if(manageFilesList(&data->socket,client_message,&buffer_size))
        {
            recvFiles(&data->socket,client_message,&rcvBytes,data->client_ID);
        }
    }
    free(client_message);
    close(data->socket);
    pthread_mutex_lock(&lock1);
    running_threads--;
    pthread_mutex_unlock(&lock1);
    printf("Closed connection with client %d\n",data->client_ID);
    free(args);
    return NULL;
}

int recvFilesList(char **buffer,int *socket,int *buffer_size)
{
    int read_size = 0,total_read=0;
    while((read_size = recv(*socket, *buffer+total_read, *buffer_size/2 , 0)) > 0) //Receive a message,of at least buffer/2 bytes,from client 
    {
        total_read += read_size;
        if(total_read + ((*buffer_size)/2) > *buffer_size) //if next read will (probably) cause an overflow,we should double our buffer's size
        {
            *buffer_size *=2;
            char *tmp_ptr = realloc(*buffer, *buffer_size);
            if(tmp_ptr == NULL) //cannot realloc memory
            {
                fprintf(stderr,"realloc failed to allocate %d bytes of memory.Server will exit\n",*buffer_size);
                return 0;
            }
            memset(tmp_ptr+total_read,0,*buffer_size-total_read);   //initialize newly allocated memory
            *buffer = tmp_ptr;
        }
        if(isComplete(*buffer,total_read))
            break;
    }
    if(checkRecvErrors(&read_size))
    {
        printf("Done reading files list\n");
        return total_read;
    }
    return 0;
}

/*
*Split message from client into tokens(del = '/') and check if the file should be synced
*/
int manageFilesList(int *socket,char *buffer,int *length)
{
    char *name_ptr,*ptr;
    char md5_client[33];  //md5 hash value is always 32 chars long plus null terminator
    int token = 0,pos = 0,size = 0,part_size = 0;
    ptr = buffer;
    while((pos < *length))
    {
        if(buffer[pos] == '/')
        {
            size = (int) (buffer+pos-ptr);
            part_size += size;
            switch(token)
            {
                case 0: //name token
                    name_ptr = malloc(size+2);	//+2 for delimiter and null terminator
                    memset(name_ptr,0,size+2);
                    strncpy(name_ptr,ptr,size);
                    break;
                case 1: //file's size token (ignored)
                    break;
                case 2: //md5 hash value from client
                    strncpy(md5_client,ptr,size);
                    md5_client[size] = 0;
                    if(shouldSync(name_ptr,md5_client))
                    {
                    	int length=strlen(name_ptr)+2;
                    	name_ptr[length-2] = '/';	//add delimiter
                    	name_ptr[length-1] = 0;		//null terminate	
                        if(send(*socket , name_ptr , length-1, 0) < 0)	//do not send null char,length-1
                        {
                            free(name_ptr);
                            fprintf(stderr,"Server failed to send data.Sync has stopped.\n");
                            return 0;
                        }
                    }
                    else
                    {
                        part_size += 2; //include 2 '/' characters
                        memset(&buffer[pos-part_size],0,part_size+1);   //erase (by setting to NULL) contents of current part,including current character
                    }
                    free(name_ptr);
                    token = -1;  //reset token counter
                    part_size = 0;  //reset partial size counter
                    break;
            }
            ptr = buffer+pos+1;
            token++;
        }
        pos++;
    }
    char EOT = 0x04;
    if(send(*socket , &EOT , sizeof(char) , 0) < 0)
    {
        fprintf(stderr,"Server failed to send data.Sync has stopped.\n");
        return 0;
    }
    return 1;
}

/**
*Reads the buffer and ignores parts that are erased(set to NULL) from manageFilesList function.
*
*/
int recvFiles(int *socket,char *buffer,int *length,int client)
{
	int i = 0,tok = 0;
	while(i < *length && *buffer == 0) //while there are NULL characters in the buffer skip them
	{
        buffer++;
        i++;
	}
	char *tok_ptr,*name_ptr,*last;
    int size = 0;
	while(i < *length)
	{
		tok_ptr = strtok_r(buffer,"/",&last); //thread safe version of strtok
		while(tok_ptr != NULL)
		{
            i += strlen(tok_ptr) + 1;
            switch(tok)
            {
                case 0: //get name token
                    name_ptr = strdup(tok_ptr); //copy the name token
                    tok++;  //go to the next token
                    break;
                case 1:
                    size = atoi(tok_ptr);   //get size token
                    tok++;
                    break;
                case 2: //md5 token
                    recvFile(socket,name_ptr,&size,client); //receive a file with name_ptr and size bytes long 
                    free(name_ptr);
                    tok = 0;
                    break;
            }
			tok_ptr = strtok_r(NULL,"/",&last);
		}
		buffer += (last-buffer);  //proceed with the next part of the message
		while(i < *length)    //skip any NULL characters
		{
            if(*buffer == 0)
            {
                buffer++;
                i++;
            }
            else
                break; 
		}
	}
    return 1;
}

/*
*Creates and locks a file(LOCK_EX= exclusive lock,no other thread can acquire lock).
*After the file is locked,data sent from client are added to the file.When the previous task is done,the file is unlocked.
*/
int recvFile(int *socket,char *name,int *fsize,int client)
{
    FILE *fptr = fopen(name, "wb");
    if(fptr == NULL)
    {
       fprintf(stderr,"Server failed to create/open file %s.\n",name);
       return 0;
    }
    int fdes = fileno(fptr);    //get file descriptor
    if(recvLockRequest(socket))
    {
        printf("Client %d requested lock.\n",client);
        while(1)
        {
            printf("Trying to lock %d\n",client);
            if(flock(fdes,LOCK_EX | LOCK_NB) == 0)    //lock the file (exclusive mode,no blocking if lock fails)
            {
                char *ptr = "ok\4";    //send 'file is locked' message followed by EOT char
                if(send(*socket,ptr,strlen(ptr),0) < 0)
                {
                    fprintf(stderr,"Server failed to send data.Sync has stopped.\n");
                    return 0;
                }
                pthread_t timer;
                struct pthread_timer *data;
                data = malloc(sizeof(struct pthread_timer));
                data->to_cancel = pthread_self();
                data->socket = *socket;
                data->fdes = fdes;
                pthread_create(&timer,NULL,startTimer,data);    //start the timer thread
                printf("Receiving file %s %d\n",name,client);
                getData(socket,fptr,fsize); //get contents of file
                pthread_cancel(timer);  //if the contents of the file were received on time then cancel timer thread
                if(flock(fdes,LOCK_UN)==0)  // after finishing those tasks, unlock it
                {
                    signalNext();   //let the next thread to acquire the lock
                    printf("Unlocked file.\n");
                }
                free(data);
                fclose(fptr);
                break;
            }
            else    //someone else has locked the file and as a result flock failed,client must be aware of that
            {
                printf("Lock failed!%d\n",client);
                char *ptr = "locked\4";    //send 'file is locked' message followed by EOT char
                if(send(*socket,ptr,strlen(ptr),0) < 0)
                {
                    fprintf(stderr,"Server failed to send data.Sync has stopped.\n");
                    return 0;
                }
                pthread_mutex_lock(&lock2);
                enqueue(&Q,pthread_self()); //add current thread to the FIFO queue
                while(!pthread_equal(pthread_self(), flag)) //wait until it's his turn to acquire the lock
                    pthread_cond_wait(&cond, &lock2);
                pthread_mutex_unlock(&lock2);
            }
        }
    }
    return 1;
}

/**
*Broadcast a signal to all waiting threads and change pthread_t flag to
*the next thread that must be executed (first thread that requested a lock)
*/
int signalNext()
{
    pthread_mutex_lock(&lock2);
    dequeue(&Q,&flag);
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock2);
    return 1;
}

/**
*Receive the rlock message from client.
*/
int recvLockRequest(int *socket)
{
    char *buffer = malloc(32);
    memset(buffer,0,32);
    int read_size = 0,total_read = 0;
    while((read_size = recv(*socket , buffer+total_read, 32 , 0)) > 0) //Receive a message,of at least 32 bytes,from client 
    {
        total_read += read_size;
        if(isComplete(buffer,total_read))
            break;
    }
    if(checkRecvErrors(&read_size))
    {
        if(strcmp(buffer,"rlock") == 0)
        {
            free(buffer);
            return 1;
        }
    }
    free(buffer);
    return 0;
}

/**
*A function that checks if any error occured after a recv operation
*read_size is the returned value of the recv function.
*/
int checkRecvErrors(int *read_size)
{
    if(*read_size == 0)
    {
        fprintf(stderr,"Client disconnected unexpectedly.\n");
        return 0;
    }
    else if(*read_size == -1)
    {
        fprintf(stderr,"recv failed\n");
        return 0;
    }
    return 1;
}

/**
*This function is used to simulate a simple timer.
*If the timer is not cancelled within TIME_TO_UNLOCK seconds,then the client thread
*will be cancelled.
*/
void *startTimer(void *args)
{
    struct pthread_timer *data = args;
    printf("Timer started!\n");
    sleep(TIME_TO_UNLOCK);
    printf("Timer done!Will cancel thread.\n");
    pthread_cancel(data->to_cancel);
    if(flock(data->fdes,LOCK_UN)==0)  // after finishing those tasks, unlock it
    {
        printf("Timer unlocked file.\n");
        signalNext();
    }
    char *ptr = "unlocked\4";    //send 'file is locked' message followed by EOT char
    if(send(data->socket,ptr,strlen(ptr),0) < 0)    //notify the client that the server is going to unlockt the file.
    {
        fprintf(stderr,"Server failed to send data.Sync has stopped.\n");
        return 0;
    }
    close(data->fdes);
    close(data->socket);
    free(args);
    return NULL;
}

/**
*Receive a file from client
*/
int getData(int *socket,FILE *fptr,int *fsize)
{
    int bf_size = 0,total_read = 0,curr_read = 0,curr_write = 0;
    char *buffer;
    if(*fsize > 2*BUFFER_SIZE)  //find a suitable size for the buffer
        bf_size = 2*BUFFER_SIZE;
    else
        bf_size = *fsize;
    buffer  = malloc(bf_size);
    if(buffer == NULL)
    {
        fprintf(stderr,"Server failed to allocate %d bytes.\n",2*BUFFER_SIZE);
        return 0;
    }
    memset(buffer,0,bf_size);
    while((curr_read = recv(*socket, buffer,bf_size,0)) > 0)
    {
        total_read += curr_read;
        if(checkRecvErrors(&curr_read))
        {
            curr_write = fwrite(buffer, sizeof(char), curr_read, fptr); //write contents of buffer to file
            if(curr_write < curr_read)  //if the bytes that were written to the file were less than available bytes,then an error occured
            {
                fprintf(stderr,"File write error.\n");
                fclose(fptr);
                free(buffer);
                return 0;
            }
            *fsize -= curr_read;    //update remaining bytes to receive
            if(*fsize - bf_size < 0)    //if the remaining bytes to read are less than the bytes we currently allow recv to read then
                bf_size = *fsize;   //read exactly the remaining amount of bytes
        }
        else
            break;
    }
    free(buffer);
    return 1;
}

/*
*This function is used to decide if a file should be synced.
*Returns 0(=false) if file is already up-to-date and 1 otherwise
*/
int shouldSync(char *name,char *md5)
{
    FILE *fptr;
    fptr = fopen (name, "rb");
    if(fptr == NULL) //file does not exist,must sync
        return 1;
    else
    {
        char s_md5[33];
        calcMD5(fptr,s_md5);
        if(strcmp(md5,s_md5) == 0)  //both files have equal md5 values
        {
            fclose(fptr);
            return 0;
        }
    }
    fclose(fptr);
    return 1;
}

/**
*Check if last character read is EOT
*If it is,then the client has nothing more to send
*/
int isComplete(char *buffer,int length)
{
    if(buffer[length-1] == 0x04)
    {
        buffer[length-1] = 0; //replace that character with null
        return 1;
    }
    return 0;
}

/**
*Function used to calculate md5 hash value of a file
*/
int calcMD5(FILE *file,char *md5)
{
    int i;
    MD5_CTX mdContext;
    int bytes;
    unsigned char val[16];  //result of md5 will be stored here
    char data[1024];
    MD5_Init (&mdContext);
    while ((bytes = fread (data, 1, 1024, file)) != 0) //read the file in chunks of 1024 bytes each
        MD5_Update (&mdContext, data, bytes); //update current md5 value
    MD5_Final (val,&mdContext);
    for(i = 0; i < 16; ++i) //convert val to hex string
        sprintf(&md5[i*2],"%02x",val[i]);
    return 1;
}

int initConnection(int *server_sck,struct sockaddr_in *client)
{
    struct sockaddr_in server; //Create a struct for server
    memset(&server, 0, sizeof(server)); //initialise server struct
    memset(&client, 0, sizeof(client)); //initialise client struct
    //build server struct
    server.sin_family = AF_INET; 
    server.sin_port = htons(SERVER_PORT);
    server.sin_addr.s_addr = INADDR_ANY;
    if((*server_sck = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) //if server's socket creation failed,print error message and terminate server
    {
        fprintf(stderr,"Cannot create socket.Server will exit.\n");
        exit(1);
    }
    printf("Server socket created successfully.\n");
    if(bind(*server_sck,(struct sockaddr *)&server , sizeof(server)) < 0)
    {
        fprintf(stderr,"Cannot bind socket.Server will exit.\n");
        exit(1);
    }
    printf("Bind operation was successful.\n");
    listen(*server_sck , 3); //listen for incoming connections
    return 1;
}