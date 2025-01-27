#ifndef FSSAPI_H
#define FSSAPI_H

#define	O_CREATE 0x1
#define O_LOCK	 0x2


int openFile( const char* pathname, int flags, /* */const char* trashdir);

int closeFile( const char* pathname);

int writeFile( const char* pathname, const char* trashdir );

int appendToFile( const char* pathname, void* buf, size_t size, const char* trashdir );

int readFile( const char* pathname, void** buf, size_t* size /*,const char* readdir*/);

int readNFiles( int n, const char* readdir);

int removeFile(const char* pathname);

int lockFile(const char* pathname);

int unlockFile(const char* pathname);

int openConnection(const char* sockname, int msec, const struct timespec abstime);

int closeConnection(const char* sockname);

int ezOpen(const char* sockname);



int SAVEfile(void* cont, size_t size,const char* pathname, const char* savedir);

int mkpath(char *dir);

#endif
