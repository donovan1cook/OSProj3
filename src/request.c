#include "io_helper.h"
#include "request.h"

#define MAXBUF (8192)

// below default values are defined in 'request.h'
int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;	

typedef struct {
    int fd;
    char filename[MAXBUF];
    int file_size;
    time_t arrival_time; 
} request_t;

request_t request_buffer[DEFAULT_BUFFER_SIZE];
int buffer_head = 0;
int buffer_tail = 0;
int buffer_count = 0;

pthread_mutex_t buffer_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

extern int scheduling_algo; 

//
//	TODO: add code to create and manage the shared global buffer of requests
//	HINT: You will need synchronization primitives.
//		pthread_mutuex_t lock_var is a viable option.
//



//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
	readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, ".%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
	strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
	strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
	strcpy(filetype, "image/jpeg");
    else 
	strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg)
{
    // TODO: write code to actualy respond to HTTP requests
    // Pull from global buffer of requests

    while (1) {
        pthread_mutex_lock(&buffer_lock);
        while (buffer_count == 0) {
            pthread_cond_wait(&buffer_not_empty, &buffer_lock);
        }

        int job_index = buffer_head;

        if (scheduling_algo ==1) {
            time_t now = time(NULL);
            int smallest = buffer_head;
            for (int i = 0; i < buffer_count; i++) {
                int spot = (buffer_head + i) % buffer_max_size;
                if (request_buffer[spot].file_size < request_buffer[smallest].file_size) {
                    smallest = spot;
                }
            }
            job_index = smallest;

            request_t picked = request_buffer[job_index];

            // Fill in gap from taking one out
            int i = job_index;
            while (i != buffer_tail) {
                int next = (i + 1) % buffer_max_size;
                request_buffer[i] = request_buffer[next];
                i = next;
            }

            // move tail 
            buffer_tail = (buffer_tail - 1 + buffer_max_size) % buffer_max_size;
            buffer_count--;

        } else if (scheduling_algo == 2) {
            int r = rand() % buffer_count;
            job_index = (buffer_head + r) % buffer_max_size;

            // Fill in gap from taking one out
            int i = job_index;
            while (i != buffer_tail) {
                int next = (i + 1) % buffer_max_size;
                request_buffer[i] = request_buffer[next];
                i = next;
            }

            // move tail 
            buffer_tail = (buffer_tail - 1 + buffer_max_size) % buffer_max_size;
            buffer_count--;
        }

        request_t req;
        if (scheduling_algo == 0) {
            req = request_buffer[buffer_head];
            buffer_head = (buffer_head + 1) % buffer_max_size;
            buffer_count--;
        } else {
            req = request_buffer[job_index];  // already removed and shifted
        }

        pthread_cond_signal(&buffer_not_full);
        pthread_mutex_unlock(&buffer_lock);

        request_serve_static(req.fd, req.filename, req.file_size);
        close_or_die(req.fd);
    }
}

//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];
    
    // get the request type, file path and HTTP version
    if (readline_or_die(fd, buf, MAXBUF) <= 0 ||
    sscanf(buf, "%s %s %s", method, uri, version) != 3) {
    request_error(fd, "unknown", "400", "Bad Request", "Malformed request line");
    return;
    }       

    // verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
	request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
	return;
    }
    request_read_headers(fd);
    
    // check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);
    
    // get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
	request_error(fd, filename, "404", "Not found", "server could not find this file");
	return;
    }
    
    // verify if requested content is static
    if (is_static) {
	if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
		request_error(fd, filename, "403", "Forbidden", "server could not read this file");
		return;
	}
    
	// TODO: directory traversal mitigation	
	// TODO: write code to add HTTP requests in the buffer

    if (strstr(filename, "..") != NULL) {
        request_error(fd, filename, "403", "Forbidden", "Directory Traversal Attempt");
        return;
    }
    
    request_t req;
    req.arrival_time = time(NULL);
    req.fd = fd;
    strcpy(req.filename, filename);
    req.file_size = sbuf.st_size;

    // locks
    pthread_mutex_lock(&buffer_lock);
    // checks if buffer is empyty
    while (buffer_count == buffer_max_size) {
        pthread_cond_wait(&buffer_not_full, &buffer_lock);
    }

    request_buffer[buffer_tail] = req;
    buffer_tail = (buffer_tail+1) % buffer_max_size;
    buffer_count++;
    //shows buffer full
    pthread_cond_signal(&buffer_not_empty);
    // unlocks
    pthread_mutex_unlock(&buffer_lock);


    } else {
	request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}
