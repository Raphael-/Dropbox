#ifndef _queue_h
#define _queue_h

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

struct queue_node {
    struct queue_node *next;    
    pthread_t data;
};
 
struct queue {
    struct queue_node *first;
    struct queue_node *last;
};

int enqueue(struct queue *q, const pthread_t value) 
{
    struct queue_node *node = (struct queue_node *)malloc(sizeof(struct queue_node));
    if (node == NULL) {
        errno = ENOMEM;
        return 1;
    }
    node->data = value;
    if (q->first == NULL)
        q->first = q->last = node;
    else {
        q->last->next = node;
        q->last = node;
    }
    node->next = NULL;
    return 0;
}
 
int dequeue(struct queue *q, pthread_t *value) 
{
    if (!q->first) {
        *value = 0;
        return 1;
    }
    *value = q->first->data;
    struct queue_node *tmp = q->first;
    if (q->first == q->last)
        q->first = q->last = NULL;
    else
        q->first = q->first->next;
 
    free(tmp);
    return 0;
}
 
void init_queue(struct queue *q) 
{
    q->first = q->last = NULL;
}
 
int queue_empty_p(const struct queue *q) 
{
    return q->first == NULL;
}


#endif