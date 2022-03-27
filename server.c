#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <pthread.h>
#include <linux/limits.h>	//PATH_MAX
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h> 

#include "./comm.h"
#include "./utils.h"
#include "./idlist.h"
#include "./filestorage.h"
#include "./errcheck.h"

FILE* Log=NULL;
pthread_mutex_t LogMutex;

#define LOCK(l)   ErrERRNO(  pthread_mutex_lock(l)   );
#define UNLOCK(l) ErrERRNO(  pthread_mutex_unlock(l) );

#define LOG(...)					\
	{	printf(__VA_ARGS__);		\
		LOCK(&LogMutex);			\
		fprintf( Log,__VA_ARGS__);	\
		fflush(Log);				\
		UNLOCK(&LogMutex);			\
		}



#define LOGOP( esit, file, sz)																								\
	{	char size[8]="0";																								\
		if(sz!=NULL) sprintf(size,"%zu", *(size_t*)sz );																					\
		LOG( "WORK: CID=%-4d""%-7s""%-10s""%s %s""\t%s\n", cid, strCmdCode(cmd.code), strReply(esit), ( sz ? size : ""),	\
		 													( sz ? "B" : ""), file ? file : ""  );			\
		}

#define RDLOCK(l)   ErrNZERO(  pthread_rwlock_rdlock(l)  );
#define WRLOCK(l)   ErrNZERO(  pthread_rwlock_wrlock(l)  );
#define RWUNLOCK(l) ErrNZERO(  pthread_rwlock_unlock(l)  );

#define OFF 0x00
#define SOFT 0x01
#define ON 0x02

volatile sig_atomic_t Status=ON;

void handlesoft(int unused){
	Status=SOFT;
	LOG("MAIN: SOFT QUIT SIGNAL RECEIVED -> NO NEW CONNECTIONS ALLOWED\n");
	return; ErrCLEANUP ErrCLEAN
	}

void  handleoff(int unused){
	Status=OFF;
	LOG("MAIN: HARD QUIT SIGNAL RECEIVED -> NO FURTHER REQUESTS WILL BE EXECUTED\n");
	return; ErrCLEANUP ErrCLEAN
	}

#define CREATE( t, att, f, arg) ErrNEG1(  pthread_create( t, att, f, arg)  );

#define ACCEPT( cid, sid, addr, l){ errno=0;				\
	cid=accept( sid, addr, l);								\
	if(cid<0){												\
		if( errno==EAGAIN) continue;						\
		else ErrFAIL;										\
		}													\
	}

#define  PSELECT(max, r, w, e, t, mask){ errno=0;			\
	nrdy=pselect(max, r, w, e, t, mask);					\
	if(nrdy<0 && errno!=EINTR) ErrFAIL;						\
	}

#define REPLY( MSG ){						\
	Reply reply;							\
	memset( &reply, 0, sizeof(Reply) );		\
	reply=MSG;								\
	WRITE( cid, &reply, sizeof(Reply) );	\
	}

#define LOGPATHN	"./serverlog.txt"
#define FIFOPATHN	"./done"
#define SOCKETPATHN "./server_sol"	//Server socket pathname		//TODO move to config file
#define MAXNUMFILES 100
#define MAXCAPACITY 1000000			//1MB
#define MAXTHREADS 4
#define BACKLOG 10

pthread_t tid[MAXTHREADS-1];		//WORKER THREADS

#define TERMINATE -2
#define DISCONN   -3



IdList* pending;	//thread safe QUEUE for fds rdy for the worker threads		M -> WTs
int done=NOTSET;				//named PIPE for fds finished by the worker threads	  WTs -> M

Storage* storage;
	
/*---------------------------------------------------WORKER----------------------------------------------------------*/	
/*-------------------------------------------------------------------------------------------------------------------*/

// called by OPEN/WRITE/APPEND becaus they may trigger a CAP LIMIT, old file/s must be sent back to the CLIENT 
int cacheAlg( int cid, Cmd cmd, size_t size){
	while( storage->numfiles == MAXNUMFILES  ||  (storage->capacity+size) > MAXCAPACITY ){
		REPLY(CACHE);
		
		File* victim=NULL;
		ErrNULL( victim=rmvLastFile() );
		
		WRITE(cid, &victim->size, sizeof(size_t));
		WRITE(cid, victim->cont, victim->size );
		WRITE(cid, victim->name, PATH_MAX);
		
		storage->capacity-=size;
		storage->numfiles--;
		
		LOGOP(CACHE,victim->name, &victim->size);
		fileDestroy( victim );
		}
//  REPLY(OK)	//the caller must respond OK to the client, which means that the replacement is complete
				//	(each one has a slightly diff routine before replying)
	SUCCESS    return  0;
	ErrCLEANUP return -1; ErrCLEAN
	}

void* work(void* unused){				//ROUTINE of WORKER THREADS
	
	while(1){
		bool Disconnecting=false;		//needed outside of the switch QUIT case, has to pass information that the CID is closing connection
		
		int cid=NOTSET;					//gets a ready ClientID (CID) from PENDING QUEUE
		while( (deqId(pending, &cid))==EMPTYLIST){	//if empty BUSY WAIT with timer
			usleep(50000);
			}
		if(cid==NOTSET) ErrSHHH;		//ERR: CID UNCHANGED means that a FATAL ERR has happened inside deqId()
		if(cid==TERMINATE) break;		//TERMINATE SIGNAL sent from the MAIN THREAD when it's time to close shop
													
		Cmd cmd={0};					//gets CMD request from CID channel
		READ(cid, &cmd, sizeof(Cmd));
		
		WRLOCK(&storage->lock);
		
		File* f=NULL;					//each OP (except READN) operates on a SPECIFIC FILE, this holds a REFERENCE to it ( getFile() )
		if(cmd.code!=READN){
		//  if(cmd.filename == NULL)	//Already checked in API call, could double check but not needed if all clients are forced to use the API		
			f=getFile(cmd.filename);
			}
		
		switch(cmd.code){
			
			case OPEN: {
				if( cmd.info & O_CREATE ){	//Client requested an O_CREATE openFile()
					if( f!=NULL){   REPLY(EXISTS); LOGOP(EXISTS,cmd.filename,NULL); break;	} //ERR: file already existing
					ErrNULL(  f=fileCreate(cmd.filename)  );	//ERR:	fatal malloc error while allocating space for new file (ENOMEM)
					ErrNEG1(  addNewFile(f)  );					//Successful CREATION	
					}	
				else{						//Client requested a normal openFile()
					if( f==NULL){   REPLY(NOTFOUND); LOGOP(NOTFOUND,cmd.filename,NULL); break;	} //ERR: file not already existing/not found
					if( findId(f->openIds, cid) ){   REPLY(ALROPEN); LOGOP(ALROPEN,cmd.filename,NULL); break; } //ERR: cid already opened this file
					}
				
				ErrNEG1(  enqId(f->openIds, cid)  );			//Successful OPEN
				
				if( cmd.info & O_LOCK ){ 	//Client requested a O_LOCK openFile()
											// mind that the the OPEN with LOCK doesnt HANG like its counterpart and returns IMMEDIATELY if unsuccessful
					if( f->lockId!=NOTSET && f->lockId!=cid ){   REPLY(LOCKED); LOGOP(LOCKED,cmd.filename,NULL); break; }
					else f->lockId=cid;							//Successful LOCK
					}
					
				REPLY(OK);   LOGOP(OK,cmd.filename,NULL);
				
				ErrNEG1(  cacheAlg(cid, cmd, (size_t)0)  );		//CACHE ALG enclosed in function bc is reused with the same code elsewhere
				
				REPLY(OK);
				
				} break;
			
			case WRITE:				// All these OPERATIONS need a similar set of REQUIREMENTS
			case APPEND:			//		mainly because they all need to be LOCKED by the CLIENT to be successful
			case REMOVE:			//      (and this implies that the FILE EXISTS in FSS and is OPEN by CID)
			case UNLOCK: {
			
				if( f==NULL){ REPLY(NOTFOUND); LOGOP(NOTFOUND,cmd.filename,NULL); break; }		//ERR: getFile() didnt FOUND it
				
				if( ! findId(f->openIds, cid) ){ REPLY(NOTOPEN); LOGOP(NOTOPEN,cmd.filename,NULL); break; } //ERR: CID hasnt OPENED it
				
				if( f->lockId != cid ){	REPLY(NOTLOCKED); LOGOP(NOTLOCKED,cmd.filename,NULL); break; }	//ERR: CID doesnt currently HOLD the FILE LOCK
				
				switch(cmd.code){
				
					case WRITE:{									//ERR: WRITE can only be performed on a NEWLY CREATED (EMPTY) FILE
						if( f->cont != NULL ){ REPLY(NOTEMPTY); LOGOP(EMPTY,cmd.filename,NULL); break; } 
						REPLY(OK);
						size_t size=0;
						READ(cid, &size, sizeof(size_t));
						printf("size: %ld\n", size);
				
						void* cont=NULL;
						ErrNULL( cont=calloc(1, size) );
						READ(cid, cont, size);
																	//ERR: if the incoming FILE cant FIT the WHOLE SPACE, it cannot be accepted in
						if( size > MAXCAPACITY ){ REPLY(TOOBIG); LOGOP(TOOBIG,cmd.filename,NULL); break; }
						
						ErrNEG1( cacheAlg(cid,cmd,size)  );
						
						f->size=size;				//Actual WRITE, storing in the FILE NODE the received CONTs
						f->cont=cont;
						storage->numfiles++;		//Updating STORAGE DIMENSIONS
						storage->capacity+=size;
				
						REPLY(OK);
						size_t* s=&size;			
						LOGOP(OK, cmd.filename, s);	//WARN: this threw 'always true' warning without this extra step
													//		(probably bc ADDR of LOCAL VAR cant be NULL)
						} break;
						
					case APPEND: {
						REPLY(OK);					//APPEND can be performed on ALL files, empty or not
				
						size_t size=0; 
						READ(cid, &size, sizeof(size_t));
				
						void* buf=NULL;
						ErrNULL( buf=calloc(1, size) );
						READ(cid, buf, size);
																	//ERR: if the RESULTING FILE cant FIT the WHOLE SPACE the append must be canceled
						if( (f->size + size) > MAXCAPACITY ){ REPLY(TOOBIG); LOGOP(TOOBIG,cmd.filename,NULL); break; }
				
						ErrNEG1(  cacheAlg(cid,cmd,size)  );
					
						void* extendedcont=NULL;
						ErrNULL(  extendedcont=realloc( f->cont, f->size+size)  );	//REALLOCATING OG FILE to fit the new CONTs
						f->cont=extendedcont;
						memcpy( f->cont + f->size, buf, size );		//APPENDING new CONTENT starting from the end of the OG FILE
						f->size+=size;				//Updating FILE SIZE to reflect the operation
				
						storage->capacity+=size;	//Incrementing STORAGE SIZE
				
						free(buf);					//the BUFFER has been COPIED, dont need it anymore
				
						REPLY(OK);
						size_t* s=&size;
						LOGOP(OK, cmd.filename, s);	//WARN: this threw 'always true' warning without this extra step (bc ADDR of LOCAL VAR cant be NULL)
						} break;
						
					case REMOVE:				
						ErrNEG1(  rmvThisFile(f)  );		//Already have all requirements, proceding with REMOVE
						LOGOP(OK, cmd.filename, &f->size);	
						ErrNEG1(  fileDestroy(f)  );		//DEALLOCATES the REMOVED FILE
						REPLY(OK);
						break;
						
					case UNLOCK:
						f->lockId=NOTSET;					//Already have all requirements, proceding with UNLOCK
						LOGOP(OK,cmd.filename,NULL);
						REPLY(OK);
						break;
					
					default: ;
					}
				} break;
			
			case READ:			//These 2 OPERATIONS instead require to NOT BEING LOCKED by OTHER CLIENTs
			case LOCK: {		//      (and this also implies that the FILE already EXISTS and that its OPEN by CID)
			
				if( f==NULL){ REPLY(NOTFOUND); LOGOP(NOTFOUND,cmd.filename,NULL); break; }
		
				if( ! findId(f->openIds, cid) ){ REPLY(NOTOPEN); LOGOP(NOTOPEN,cmd.filename,NULL); break; }
		
				if( f->lockId!=NOTSET && f->lockId!=cid ){ REPLY(LOCKED); LOGOP(LOCKED,cmd.filename,NULL); break; } //ERR: FILE LOCKED by OTHER CID
				
				switch(cmd.code){
				
					case LOCK:										//ERR: No sense in GETTING the LOCK AGAIN if the CLIENT already owns it
						if( f->lockId==cid ){ REPLY(ALRLOCKED); LOGOP(ALRLOCKED,cmd.filename,NULL); break; }

						f->lockId=cid;							//Proceeding with the LOCKING
		
						LOGOP(OK,cmd.filename,NULL);
						REPLY(OK);
						break;
					
					case READ:
						if( f->cont==NULL ){ REPLY(EMPTY); LOGOP(EMPTY,cmd.filename,NULL); break; }		//ERR: no sense in READING an EMPTY file
				
						REPLY(OK);
				
						WRITE(cid, &f->size, sizeof(size_t));	//Sending the requested DATA back
						WRITE(cid, f->cont, f->size);
				
						LOGOP(OK,cmd.filename,&f->size);
						break;
					
					default: ;
					}
				} break;
						
			case (CLOSE):{
			
				if( f==NULL){   REPLY(NOTFOUND); LOGOP(NOTFOUND,cmd.filename,NULL); break; }		//ERR: file not found
				
				if( f->lockId==cid) f->lockId=NOTSET;		//Eventual UNLOCK before closing
				
				if( findRmvId(f->openIds, cid)  ){  REPLY(OK);  LOGOP(OK,cmd.filename,NULL); }
				else {  REPLY(NOTOPEN); LOGOP(NOTOPEN,cmd.filename,NULL); }
															//ERR: if the FILE wasnt already OPEN by CID, there was no sense in CLOSING it
				} break;		

			case (READN):{
				REPLY(OK);		//READN basically CANT FAIL by design (worst case scenario other than FATAL SC ERROR is that it reads 0 files)
				
				File* curr=storage->last;
				
				int n=( cmd.info<=0  ?  storage->numfiles  :  cmd.info );
				int n_read=0;
				
				for(int i=0; i<n; i++){		//for each of N RANDOM FILES
					if( curr==NULL) break;										//If FSS is completely EMPTY get out
					
					if( curr->lockId!=NOTSET && curr->lockId!=cid ) continue;	//READN cannot read LOCKED files
					
					if( curr->cont==NULL ) continue;							//Also no sense in reading EMPTY files
					
					REPLY(ANOTHER);								//Tells the CLIENT that there's ANOTHER FILE to be READ
					WRITE(cid, &curr->size, sizeof(size_t));
					WRITE(cid, curr->cont, curr->size);
					WRITE(cid, curr->name, PATH_MAX);			//Must also send his NAME other than SIZE and CONT
					
					LOGOP(ANOTHER, curr->name , &curr->size);		
					n_read++;									//increments READ COUNT
					curr=curr->prev;							//CONTINUES TRAVERSING the LIST
					}
				
				REPLY(OK);
				LOGOP(OK, NULL, NULL);
				break;
				} break;			
				
	
			case (IDLE):
			case (QUIT):{
				File* curr=storage->last;
			
				for( int i=0; i < storage->numfiles; i++){	//FOR EACH FILE in the FSS:
					if( curr==NULL) break;							//safety check for END of the LIST		
					
					findRmvId(curr->openIds, cid);					//UNLOCKS all files locked by CID	
					if( curr->lockId==cid ) curr->lockId=NOTSET;	//CLOSES all files open by cid 
										
					curr=curr->prev;								//Traverse the FSS					
					}
				Disconnecting=true;		//Sets VAR so that the CID isnt passed back to the MAIN THREAD
				} break;
			
			default:
				fprintf(stderr, "Come sei finito qui? cmd code: %d\n", cmd.code);
				break;		
			}

		RWUNLOCK(&storage->lock);
		
		if(Disconnecting){
			LOG("WORK: CLIENT %d DISCONNECTED\n", cid);
			cid=DISCONN;						//A CODE is SENT back to the MAIN THREAD instead of the DISCONNECTED CLIENT ID
			}									//		(decrements the number of active clients in main)

		WRITE( done, &cid, sizeof(int) );		//if it isnt DISCONNECTING the CLIENT ID is SENT BACK to the MAIN THREAD
		
		printf("STORAGE:  ----------------------------\n");		//DBG: Prints the list of FILES currently in the FSS
		storagePrint();
		printf("--------------------------------------\n\n");
		fflush(stdout);
		}

	SUCCESS    return NULL;
	ErrCLEANUP return NULL; ErrCLEAN
	}

int spawnworker(){								//worker threads SPAWNER
	for(int i=0; i<MAXTHREADS-1; i++){
		CREATE(&tid[i], NULL, work, NULL );		//WORKER THREAD
		LOG("MAIN: CREATED WORKER THREAD #%d\n", i);
		}

	return 0;
ErrCLEANUP
	return -1;
ErrCLEAN
	}
	
/*-----------------------------------------------------MAIN----------------------------------------------------------*/	
/*-------------------------------------------------------------------------------------------------------------------*/

int main(){

	unlink(SOCKETPATHN);								//remove server socket if already exists FIXME
	
	
	/*------------------------Signals--------------------------*/
	sigset_t mask, oldmask;							//SIGNAL MASKS
	ErrNEG1(  sigemptyset(&mask)  );   
	ErrNEG1(  sigaddset(&mask, SIGINT)  ); 
	ErrNEG1(  sigaddset(&mask, SIGQUIT) );
	ErrNEG1(  sigaddset(&mask, SIGHUP)  );
	
	ErrNEG1(  sigaddset(&mask, SIGPIPE) );
	ErrNZERO(  pthread_sigmask(SIG_BLOCK, &mask, &oldmask)  );	//blocks signals during SERVER INITIALIZATION
	ErrNEG1(  sigaddset(&oldmask, SIGPIPE)  );					//blocks SIGPIPE during the wole EXECUTION (both on mask & oldmask)
	
	struct sigaction off={0}, soft={0};
	off.sa_handler=&handleoff;
	soft.sa_handler=&handlesoft;
	off.sa_mask=soft.sa_mask=mask;					//blocks signals inside SIG HANDLER function
	
	ErrNEG1(  sigaction(SIGINT,  &off, NULL)  );				//assigns each SIG to the its SIG HANDLER
	ErrNEG1(  sigaction(SIGQUIT,&soft, NULL)  );	// /!\ temp quit in softquit bc it has a keyboard shortcut
	ErrNEG1(  sigaction(SIGHUP, &soft, NULL)  );
	
	/*-------------------------Log-----------------------------*/
	

//	FILE* Log=NULL;  moved to global
	ErrNULL(  Log=fopen( LOGPATHN, "w")  );
//  pthread_mutex_t LogMutex;
	ErrERRNO(  pthread_mutex_init(&LogMutex, NULL)  );
	
	/*------------------------Socket---------------------------*/
	int sid=NOTSET;
	ErrNEG1(  sid=socket(AF_UNIX, SOCK_STREAM, 0)  );	//SID: server id (fd), SOCKET assigns it a free channel
	
	struct sockaddr_un saddr;							//saddr: server address, needed for BIND
	memset(&saddr, 0, sizeof(saddr));
	saddr.sun_family=AF_UNIX;							//adds domain and pathname to the server address structure
	strcpy(saddr.sun_path, SOCKETPATHN);				
	
	ErrNEG1(  bind(sid, (struct sockaddr*) &saddr, SUN_LEN(&saddr))  );	//BIND: NAMING of the opened socket
	
	ErrNEG1( listen(sid, BACKLOG)  );							//sets socket in LISTEN mode
	
	LOG("MAIN: SOCKET OPENED\n");
	
	/*-------------------Pending-queue/Done-pipe---------------*/
	ErrNULL(  pending=idListCreate()  );				//PENDING queue
	
	if( mkfifo("./done", 0777) != 0){				//DONE named pipe
		if( errno != EEXIST){
			perror("Error: couldnt create named pipe\n");
			ErrSHHH;
			}
		}
	ErrNEG1(  done=open("./done", O_RDWR)  );
	
	/*-------------------------FD-sets-------------------------*/
	fd_set all, rdy;								//ALL set of FDs controlled by select()
	FD_ZERO(&all);

	FD_SET(sid, &all);									//adding SID to ALL
	int maxid=sid;
	
	FD_SET(done, &all);									//adding DONE to ALL
	if(done > maxid) maxid=done;
	
	/*---------------------FSS-/-Threads-----------------------*/	
	ErrNEG1(  storageCreate()  );		//allocates an empty FILE STORAGE
	
	ErrNEG1(  spawnworker()  );			//spawns WORKER THREADS

	LOG("MAIN: SERVER READY\n");
	
	
	/*---------------------Manager-loop------------------------*/
	int activeCid=0;
	
	while( Status!=OFF ){
		rdy=all;
		int nrdy=0;	//select returns the number of rdy fds				//select "destroys" the fd that arent rdy
		/*nrdy=*/PSELECT(FD_SETSIZE, &rdy, NULL, NULL, NULL, &oldmask);		// pselect( n, read, write, err, timeout, signals)
		
		if( nrdy>0){							//if <=0 the select has been triggered by one of the installed signals and the FD cycle is skipped
			for(int i=0; i<=maxid; i++){
				if( FD_ISSET( i, &rdy) ){
					if( i==sid && Status==ON){				//the server socket (sid) has a pending request and is ready to accept it
						int cid;							//	but only if the server is not in SOFT QUIT mode (Status==NOCONN)
						ACCEPT(cid, sid, NULL, NULL );
						FD_SET(cid, &all);
						if(cid>maxid) maxid=cid;
						activeCid++;
						LOG("MAIN: ACCEPTED NEW CLIENT %d\n", cid);
						}
					else if( i==done){			//the named pipe containing the finished requests has a cid that must return in the controlled set
						int cid;
						READ(done, &cid, sizeof(int));
						if( cid==DISCONN ){
							activeCid--;
							}
						else FD_SET(cid, &all);
							//printf("received back client %d\n", cid);
						}
					else{									//one of the clients has something that the server has to read
						FD_CLR( i, &all);
						enqId(pending, i);
						}				
					}
				}
			}
		if( Status==SOFT && activeCid==0) break; 
		}

	for( int i=0; i<MAXTHREADS-1; i++)
		enqId(pending, TERMINATE);
		
	for( int i=0; i<MAXTHREADS-1; i++){
		ErrERRNO(  pthread_join( tid[i], NULL)  );
		LOG("MAIN: THREAD #%d JOINED\n", i);
		}
	
	ErrNEG1(  storageDestroy()       );
	ErrNEG1(  unlink(FIFOPATHN)      );
	ErrNEG1(  close(done)            );
	ErrNEG1(  idListDestroy(pending) );
	ErrNEG1(  unlink(SOCKETPATHN)    );
	ErrNEG1(  close(sid)             );
	LOG("MAIN: SERVER SUCCESSFULLY CLOSED\n");
	ErrERRNO(  pthread_mutex_destroy(&LogMutex) );
	ErrNZERO(  fclose(Log)  );
	return 0;
	
ErrCLEANUP
	if(storage) storageDestroy();
	unlink(FIFOPATHN);
	if(done!=NOTSET) close(done);
	if(pending) idListDestroy(pending);
	unlink(SOCKETPATHN);
	if(sid!=NOTSET) close(sid);
	pthread_mutex_destroy(&LogMutex);
	LOG("MAIN: SERVER ERROR (CLEANUP DONE)\n");
	fflush(Log);
	if(Log) fclose(Log);
	exit(EXIT_FAILURE);
ErrCLEAN
	}
