#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dirent.h> 
#include <sys/stat.h>
#include <openssl/md5.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "FileSynchronizationClient.h"

//client must be aware of server's port number
#define SERVER_PORT 12010
#define BUFFER_SIZE 512

int main(int argc, char ** argv)
{
    if(argc != 2)
    {
    	fprintf(stderr,"Hostname argument is missing.Client will exit.\n");
    	exit(1); //1 indicates unsucessful termination
    }
    run(argv[1]);
    return 0;
}

int run(char *hostname)
{
    chdir("./dropbox/");    //change current directory because stat function is searching inside current directory only
    char *server_req = malloc(BUFFER_SIZE);
    if(server_req == NULL)
    {
        fprintf(stderr,"Cannot allocate %d bytes for buffer\n",BUFFER_SIZE);
        return 0;
    }
    int socket = 0;
    while(1)
    {
        if(initConnection(&socket,hostname))
        {
            if(!sync_(&socket,&server_req)) //sync was not successful
            {
                printf("Client will exit.\n");
                break;
            }
            close(socket);
        }
        else
            break;
        sleep(15); //sleep 15 seconds
    }
    free(server_req);
    return 1;
}

int sync_(int *socket,char **buffer)
{
    printf("Sync with server has started.\n");
    if(sendFileList(socket))
    {
        memset(*buffer,0,BUFFER_SIZE);
        int buffer_size = BUFFER_SIZE;
        if(recvRequests(socket,buffer,&buffer_size))
            if(sendFiles(socket,*buffer))
                return 1;
    }
    return 0;
}

/**
*Establish a connection with the server
*/
int initConnection(int *sck,char *hostname)
{

    struct hostent *he; //struct used to resolve hostname to IP address

    if ((he = gethostbyname(hostname)) == NULL) //if resolve failed,print error message and exit
    {
        fprintf(stderr,"Cannot resolve hostname %s.Client will exit.\n",hostname);
        return 0;
    }

    printf("Hostname resolution done.\n");
    struct sockaddr_in server; 
    memset(&server, 0, sizeof(server)); //initialise server struct

    //build server struct
    server.sin_family = AF_INET; 
    server.sin_port = htons(SERVER_PORT);
    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);  // copy network address part of he struct to server struct
    *sck = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); //create client's socket

    if (*sck < 0) //if socket creation failed,print error message and terminate client
    {
        fprintf(stderr,"Cannot create socket.Client will exit.\n");
        return 0;
    }
    printf("Socket created successfully.\n");
    //Try to connect to the server
    if (connect(*sck, (struct sockaddr *)&server, sizeof(server)) < 0) //if connection failed
    {
        fprintf(stderr,"Cannot connect to server %s at port %d.Client will exit.\n",hostname,SERVER_PORT);
        return 0;
    }
    printf("Connection established.Server: %s (IP: %s) at port: %d\n",hostname,inet_ntoa(*(struct in_addr*)he->h_addr),SERVER_PORT);
    return 1;
}

/**
*Send name,length and md5sum of all files in current directory(dropbox directory)
*/
int sendFileList(int *socket)
{
  DIR *d;
  FILE *fptr;
  struct dirent *dir;
  struct stat fileStat;
  char md5_hex[32];
  d = opendir("./");
  if(d)
  {
    while((dir = readdir(d)))
    {
      if(dir->d_type != DT_DIR )
      {
        if(stat(dir->d_name,&fileStat) < 0)   
        {
          fprintf(stderr,"Cannot retrieve informations about file %s.File will be ignored\n",dir->d_name);
          continue;
        }
        else
        {
          printf("Checking file %s ...",dir->d_name);
          fptr = fopen (dir->d_name, "rb");
          if (fptr == NULL)
          {
            fprintf(stderr,"Client failed to open file %s.Sync has stopped.Close this file and press enter to retry.\n",dir->d_name);
            return 0;
          }
          size_t sz;  //holds the length of the string to send (including NULL terminator)
          calcMD5(fptr,md5_hex);
          sz = snprintf(NULL,0,"%s/%d/%s/", dir->d_name,(int)fileStat.st_size,md5_hex) + 1; //snprintf will return the length of the string to send (excluding NULL terminator)
          char *msg = malloc(sz); //allocate sz bytes of memory using malloc
          memset(msg, 0, sz);  //clears the buffer
          snprintf(msg,sz,"%s/%d/%s/", dir->d_name,(int)fileStat.st_size,md5_hex); //store string at msg buffer
          if(!sendMessage(socket,msg))
          {
            free(msg);
            return 0;
          }
          free(msg);  //free dynamically allocated memory
          fclose(fptr);
          printf("\n");
        }
      }
    }
    closedir(d);
    sendMessage(socket,"\4");    //send EOT(=End Of Transmission,special ASCII char)
  }
  else
  {
    printf("Cannot open dropbox directory.Sync has stopped.\n");
    return 0;
  }
  return 1;
}

int recvRequests(int *socket,char **buffer,int *length)
{
	int read_size=0,total_read=0;
	while((read_size = recv(*socket , *buffer+total_read, *length/2 , 0)) > 0) //Receive a message,of at least buffer/2 bytes,from client 
	{
		total_read += read_size;
		if(total_read + (*length/2) > *length) //if next read will (probably) cause an overflow,we should double our buffer's size
		{
			*length *=2;
			char *tmp_ptr = realloc(*buffer, *length);
			if(tmp_ptr == NULL) //cannot realloc memory
			{
				fprintf(stderr,"realloc failed to allocate %d bytes of memory.Server will exit\n",*length);
                free(buffer);
				return 0;
			}
            memset(tmp_ptr+total_read,0,*length-total_read);
			*buffer = tmp_ptr;
		}
		if(isComplete(*buffer,total_read))
			break;
	}
    if(checkRecvErrors(&read_size))
    {
        if(*buffer[0] == 0)  //First char of buffer is NULL,that means all files are up to date (server only sent EOT char)
        {
            printf("All files are up to date\n");
        }
        return 1;
    }
    return 0;
}

/**
*Receive requested files from server
*/
int sendFiles(int *socket,char *req_buffer)
{
	int size = BUFFER_SIZE*2;
	char *file_buffer = malloc(size);
	memset(file_buffer,0,size);
	if(file_buffer == NULL)
	{
		fprintf(stderr,"Cannot allocate %d bytes for buffer\n",size);
    	exit(1);
	}
	char *tok_ptr = strtok(req_buffer,"/");
	FILE *fptr;
	int read_size = 0,total = 0,pending = 0;
	while (tok_ptr != NULL)
  	{
  		total = 0;
		fptr = fopen (tok_ptr, "rb");
		if(fptr == NULL)
		{
			printf("Server requested for a file that doesn't exist\n");
            free(file_buffer);
			return 0;
		}
        if(sendMessage(socket,"rlock\4")) //send lock request
        {
            handleLocks(socket,tok_ptr);
        }
		while((read_size = fread(file_buffer, sizeof(char), size, fptr))>0)
        {
        	total += read_size;
            if(send(*socket, file_buffer, read_size, 0) < 0)
            {
                printf("Failed to send data\n");
                return 0;
            }
            ioctl(*socket, FIONREAD, &pending); //check for data to read from socket
            if(pending != 0)    //if there is something to read
                if(!handleLocks(socket,tok_ptr))    //server sent a message about file locks (timeout occurred)
                {
                    fclose(fptr);
                    free(file_buffer);
                    return 0;
                }
        }
       	printf ("Done sending file %s.Total bytes sent %d\n",tok_ptr,total);
        fclose(fptr);
		tok_ptr = strtok (NULL, "/");
  	}
    free(file_buffer);
  	return 1;
}

/**
*Receive messages from server about file locks status
*/
int handleLocks(int *socket,char *curr_file)
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
        if(strcmp(buffer,"ok") == 0)    //acquired locked for file
        {
            printf("Acquired lock for file %s.\n",curr_file);
            free(buffer);
            return 1;
        }
        else if(strcmp(buffer,"locked") == 0)   //file is already locked by another client
        {
            while(1)
            {
                printf("File %s is locked.Waiting for unlock message..\n",curr_file);
                memset(buffer,0,32);
                total_read = 0;
                //client will block until server sends an unlock message
                while((read_size = recv(*socket , buffer+total_read, 32 , 0)) > 0) //Receive a message,of at least 32 bytes,from client 
                {
                    total_read += read_size;
                    if(isComplete(buffer,total_read))
                        break;
                }
                if(checkRecvErrors(&read_size))
                {
                    if(strcmp(buffer,"ok") == 0)
                    {
                        printf("File %s is now unlocked.\n",curr_file);
                        free(buffer);
                        return 1;
                    }
                }
            }
        }
        else if(strcmp(buffer,"unlocked") == 0) //timeout occurred,server will unlock the file
        {
            printf("WARNING!Server will unlock file %s.Maybe the file is too big or the connection too slow.Sync has stopped.\n",curr_file);
        }
    }
    free(buffer);
    return 0;
}

int checkRecvErrors(int *read_size)
{
    if(*read_size == 0)
    {
        fprintf(stderr,"Server disconnected unexpectedly.\n");
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
*Check if last character read is EOT
*If it is,then the server has nothing more to send
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

/*
*Sends an application layer message to server
*/
int sendMessage(int *socket,char *msg)
{
    if(send(*socket , msg , strlen(msg) , 0) < 0) //if client was unable to send
    {
        fprintf(stderr,"Client failed to send data.Sync has stopped.\n");
        return 0;
    }
    return 1;
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