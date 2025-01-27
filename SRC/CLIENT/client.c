#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdbool.h>

#include <api.h>
#include <utils.h>		//READ(), WRITE(), FREAD(), FWRITE()
#include <errcheck.h>
#include <optqueue.h>

#include <comm.h>

#define SOCKETFILENAME "server_sol"
#define TEMPFOLDER "./TEMP/"
#define SOCKETPATH TEMPFOLDER SOCKETFILENAME

static int PRINT = 0;

static OptQueue* optQueue;

//Used for API calls inside the main command execution while loop
//on ERROR: PRINTS API  BREAKS out of switch loop, PRINTS ERROR and SLEEPS (as much as required by -t OPTION) / on SUCCESS: SLEEPS anyway

int errAPI( const char* op, int ret, Opt* curr, char* file, struct timespec* sleeptime) 
{
	if( (ret)==-1 )
  {
		if (PRINT) fprintf( stderr, "%s: ERROR: (-%c) %s FAILED (ERRNO=%d) %s\n", op, curr->cmd, file, errno, strerror(errno) );
		nanosleep(sleeptime, NULL);
		return -1; // break;
	}
	else 
  {
    if(PRINT) printf("%s: SUCCESS", op);
    nanosleep(sleeptime, NULL);
    return 0;
  };
}

//like the previous one but CONTINUES (eg: inside while )
int errSDIR( int n_reads, const char* op, struct dirent* ent, struct timespec* sleeptime)
{
  if( (n_reads)==-1)
  {
		fprintf( stderr, "Error: %d on file %s FAILED (ERRNO=%d) %s\n", n_reads, ent->d_name, errno, strerror(errno) );
    if (PRINT) printf("%s: ERROR\n", op);
		nanosleep(sleeptime, NULL);
		return -1; // continue;
	}
	else
  {
    if(PRINT) printf("%s: ERROR\n", op);
    nanosleep(sleeptime, NULL);
    return 0;
  }
}

char* trashdir=NULL;
int n_w=-1;
struct timespec sleeptime;

static inline double timespectot( struct timespec* t){
	return t->tv_sec+(t->tv_nsec/1000000000.0);
	}

int SENDdir(char* senddir) //used in -w option
{
	int n_sent=0;				//counts the num of SENT files
	
	DIR* dir=NULL;
	ErrNULL(  dir=opendir(senddir)  );
	
	struct dirent* ent;				
	while( (errno=0, ent=readdir(dir)) != NULL && n_w!=0)
  {
		//if( strcmp(ent->d_name,".")!=0  ||  strcmp(ent->d_name,"..")!=0 ) continue;
		
		if( strlen(senddir)+1+strlen(ent->d_name) > PATH_MAX-1 ){
			fprintf(stderr,"Error: resulting pathname longer than %d chars, file '%s' wont be saved\n", PATH_MAX-1, ent->d_name);
			continue;
			}
		
		char entpathname[PATH_MAX];
		memset( entpathname, '\0', PATH_MAX);
		strcpy( entpathname, senddir );
		strcat( entpathname, "/");
		strcat( entpathname, ent->d_name);
	
		struct stat entstat;
		ErrNEG1( stat(entpathname, &entstat)  );
		
		if( strcmp(ent->d_name,".")!=0  &&  strcmp(ent->d_name,"..")!=0 )
    {
			if( S_ISDIR(entstat.st_mode) ) n_sent+= SENDdir(entpathname);
			else
      {
				n_w--;			//with these increments here the behaviour is:	do N WRITE ATTEMPTS
				n_sent++;		//if these are moved after the 3 API CALLS:		do N SUCCESSFUL WRITES
				if(PRINT) printf("\n\tCREATE>WRITE>CLOSE of FILE %s\n", entpathname);
				
        int err = 0;

        err = openFile(  entpathname, O_CREATE | O_LOCK, trashdir);
        if( errSDIR( err, "OPEN", ent, &sleeptime) ) continue;
      
        err = writeFile( entpathname, trashdir);
        if( errSDIR( err, "WRITE", ent, &sleeptime) ) continue;

				err = closeFile( entpathname);
        if( errSDIR( err, "CLOSE", ent, &sleeptime) ) continue;
      }
		}
	}

	ErrNZERO( errno );
	
	ErrNEG1(  closedir(dir)  );
	return n_sent;
		
ErrCLEANUP
	if(dir) closedir(dir);
	return -1;
ErrCLEAN
	}

int main (int argc, char **argv){

	char* 	socketname=NULL;
/*	bool */	PRINT=false;		//moved to API.h bc its needed inside the API to print the SIZEs on some actions
	
	bool found_h=false;
	bool found_f=false;
	bool found_p=false;
	
	ErrNEG1(  optQueueCreate()  );
	
	int opt;
	while( (opt=getopt(argc, argv, ":w:W:a:r:R::l:u:c:d:D:t:f:ph"))!= -1){	/*
															: 	-> 1 argomento obbligatorio
															:: 	-> 1 argomento opzionale
															"ph"-> nessun argomento
															-1	-> finite le opzioni
															
															first ':' -> serve per riconoscere il "missing argument" ma funziona solo sull'ultima opt
																		 le precedenti prendono come arg la opt successiva
															
															optarg: contiene un ptr all'argomento dell'opzione corrente
															optind: posizione corrente della funzione getopt sul vettore argv[]
															optopt: se getopt incontra un carattere non riconosciuto viene memorizzato qui */
		switch(opt){
			case 'w':
            case 'W':
            case 'a':
            case 'r':
            case 'l':
            case 'u':
            case 'c':
            case 'd':
            case 'D':
            case 't':									//-R uses the optional argument '::' => 'optarg' can be NULL and it's correct behaviour
            case 'R':									// /!\ only works if the arg is adjacent to '-R' ES OK:'-R3'  ERR:'-R 3' 3 unknown arg
            	enqOpt( opt, optarg);
            	break;			
				
			case 'f':
				if( found_f ){ fprintf(stderr,"Warning: socket pathname already specified, ignored\n"); break; }
				socketname=optarg;
				found_f=true;
				break;
			
			case 'p':
				if( found_p){ fprintf(stderr,"Warning: print mode already specified, ignored\n"); break; }
				PRINT=true;
				found_p=true;
				break;
						
			case 'h':
				if( found_h){ fprintf(stderr,"Warning: help prompt already visualized\n"); break; }
				
				found_h=true;
				printf(
					"------------------------------------------------------------------------------------------------------------------"
					"WARNING:           All options/actions are executed in the order in which are written (except -f -p -h)\n"
					"OPTIONS:\n"
					" -f socketname      Specifies the socket name used by the server [important] (default='./server_sol')\n"
					" -d dirname         Updates folder where to save the files read by the -r and -R commands (optional)\n"
					" -D dirname         Updates folder where to save files ejected from the server, for use with -w and -W (optional)\n"
					" -t time            Changes time in milliseconds between two consecutive requests (optional) (default=0)\n"
					" -p                 Enable verbose mode (optional)\n"
					"ACTIONS:\n"
					" -w dirname[,<n>]   Writes all files in 'dirname' to the server; if present stops at 'n' files sent (0 or absent means send all)\n"
					" -W f1[,f2,f3...]   Sends the specified file list to the server\n"
					" -r f1[,f2,f3...]   Reads a list of files from the server\n"
					" -R[<n>]            Reads 'n' files from the server; if 'n' is unspecified or <=zero reads all of them;\n"
					" -l f1[,f2,f3...]   Acquire read/write lock of the specified file list\n"
					" -u f1[,f2,f3...]   Releases read/write lock of the specified file list\n"
					" -c f1[,f2,f3...]   Deletes the specified file list from the server\n"
					"HELP:\n"
					" -h                 Print this message once\n"
					"------------------------------------------------------------------------------------------------------------------");
				break;
				
			 case ':':		
  				printf("L'opzione '-%c' richiede un argomento\n", optopt);
  				return -1; break;
			
			case('?'):
				printf("Opzione '-%c' non riconosciuta.\n", optopt);
				return -1; break;		
			
			default:
				printf("Come sei finito qui?\n");
				errno=EPERM; break;	
			}
		}
	
	
	
	
	
	
	if(optind < argc){
		fprintf(stderr,"Unknown options/arguments are present:\n");
		while(optind < argc){
			printf("%s\n", argv[optind]);
			optind++;
			}
		fprintf(stderr,"Reminder: option '-R' uses the optional arg function included in getopt() spec.\n"
					   "\t'-R   ': read all files\n"
					   "\t'-R3  ': read at most 3 files\n"
					   "\t'-R 3 ': '3' not recognized, could break all subsequent cmd line options, behaves like '-R  '\n"); 
		}


	char* readdir=NULL;
/*	char* */trashdir=NULL;														//moved to global bc used inside -w auxiliary function ( SENDdir() )
	
/*	static struct timespec sleeptime;	*///INTERVAL time between two API calls	//moved to global bc used inside -w SENDdir()
	sleeptime.tv_sec=0;
	sleeptime.tv_nsec=0;
	
	void* readbuf=NULL;
	size_t readsize=0;
	
	
	if(socketname==NULL) socketname=SOCKETPATH;	//TODO choose if leaving this here or force -f option to specify the socketname
	ErrNEG1(  openConnection(socketname, 0, sleeptime)  );		//ignoring the timed connection for now
	
	
	Opt* curr=NULL;
	while(	(curr=deqOpt()) !=NULL){
		if(PRINT) printf("------------------ -%c:\n", curr->cmd);
		char* file=NULL; 		//used in the nested while loop when tokenizing each argument in single files
		int r=-1;				//used as RETURN VALUE in most API CALLS: needed to store a RV for PRINT MODE
		
		char* argcpy=NULL;
		if(curr->arglist!=NULL)
			argcpy=strdupa(curr->arglist);		//AUTOMATIC STRDUP noice
		
		switch(curr->cmd)
    {
      case 't':		// -t updates sleeptime timer (in milliseconds)
				int msec=0;
				if( !PRINT ) sleeptime.tv_sec=  msec/1000;
					sleeptime.tv_nsec=(msec%1000)*1000000;
					
					if(PRINT) printf("CMD: Updated TIME DELTA: %f\n", timespectot(&sleeptime) );
			  break;	
		
			case 'd':		// -d updates readdir directory path
				readdir=strdupa(argcpy); 
				if(PRINT) printf("CMD: Updated READ DIRECTORY: %s\n", readdir );
			  break;
			
			
			case 'D':		// -D updates trashdir directory path
				trashdir=strdupa(argcpy);
				if(PRINT) printf("CMD: Updated CACHE EJECTION DIRECTORY: %s\n", readdir );				
			  break;
			
			
			
			case 'R':{		// -R has an optional number argument
				int n_R=-1;		//default behaviour: read all files
				if( argcpy != NULL){
					if( !isNumber(argcpy) ){
						fprintf(stderr,"Warning: argument of '-R' is NaN\n");
						break;
						}
					else{
						n_R=atoi(argcpy);		//if n specified correctly: send at most n files
						if(n_R==0) n_R=-1;		// because 0 means send all files
						}
					}
				
				if(PRINT) printf("REQ: BULK READ of %s FILES from FSS\n\n", n_R<0 ? "ALL" : argcpy );
				if( errAPI("READ", readNFiles( n_R, readdir), curr, file, &sleeptime) ) break;

			case 'w':		// -W has a single arg or 2 comma separated args
				char* senddir=strtok_r(argcpy,",",&argcpy);	//it's impossible that is NULL
				char* n_str=strtok_r(argcpy,",",&argcpy);
				
				if( strtok_r(argcpy,",",&argcpy) != NULL ){
					fprintf(stderr,"Error: -W option has more than 2 arguments in its argument list\n");
					break;
					}
					
			  /*  int n_w;	*/					//moved to global bc used inside -w SENDdir()
				n_w=-1;					//default behaviour: send all files (gets decremented indefinitely inside SENDdir)
				if( n_str!=NULL){
					if( !isNumber(n_str) ){
						fprintf(stderr,"Warning: second arg of '-W' is NaN\n");
						break;
						}													       
					else{
						n_w=atoi(n_str);		//if n specified correctly: send at most n files
						if(n_w==0) n_w=-1;		// because 0 means send all files
						}
					}
					
				if(PRINT) 
          printf
            ( "REQ: FULL DIR WRITE of at most %s FILES from DIR %s\n"
            , n_w<0 ? "ALL" : argcpy
            , senddir
            );
				if( errAPI( "SEND", SENDdir(senddir), curr, file, &sleeptime) ) break; 
			  break;
			
			
			
			case 'a':{		// -a has 2 comma separated args
				ErrLOCAL
				char* src=strtok_r(argcpy,",",&argcpy);
				char* dest=strtok_r(argcpy,",",&argcpy);
				if( dest==NULL){
					fprintf(stderr,"Error: target file missing from -a option\n");
					break;
					}
				if( strtok_r(argcpy,",",&argcpy) != NULL ){
					fprintf(stderr,"Error: -a option has more than 2 arguments in its argument list\n");
					break;
					}
				
				FILE* source=fopen(src, "r");		
				if(source==NULL){								//ERR: file not found or other (fopen SETS ERRNO)
					if(errno==ENOENT){
						fprintf(stderr, "Error: file to append doesnt exist, skipping command\n");
						break;
						}
					else ErrFAIL;
					}

				struct stat srcstat;
				ErrNEG1(  stat(src, &srcstat)  );			//ERR: filestat corrupted	(stat SETS ERRNO)
				size_t size=srcstat.st_size;
	
				void* cont=NULL;
				ErrNULL(  cont=calloc(1, size)  );		//ERR: malloc failure (malloc SETS ERRNO=ENOMEM)
				FREAD(cont, size, 1, source);					//ERR: file read failure (freadfull SETS ERRNO=EIO)
				ErrNZERO(  fclose(source)  );					//ERR: fclose() returns EOF on error (SETS ERRNO)
				
				if (PRINT) printf("API: APPEND FILE %s to FILE %s", src, dest);
				if( errAPI("APPEND",appendToFile( dest, cont, size, trashdir),curr,file, &sleeptime) ) break;

			SUCCESS
				break;
			ErrCLEANUP
				if(source){
					fflush(source);
					fclose(source);
					}
				break;
			ErrCLEAN		
				}
				
			
			
			default:{		// -w -r -l -u -c  all these have a comma separated list of arguments
				char* file=NULL;
				while( (file=strtok_r( argcpy, ",", &argcpy)) !=NULL ){		//FOR EACH FILE in its arglist:

					switch(curr->cmd){										//call the API function relative to the current OPT
						case 'W':
							if(PRINT) printf("REQ: CREATE>WRITE>CLOSE of FILE %s\n", file);
							
							if( errAPI( "OPEN",  openFile  (file, O_CREATE | O_LOCK, trashdir), curr, file, &sleeptime) ) break;
							if( errAPI( "WRITE", writeFile (file, trashdir), curr, file, &sleeptime) ) break;
							if( errAPI( "CLOSE", closeFile (file), curr, file, &sleeptime) ) break;

							break;
						
						case 'r':
							if(PRINT) printf("REQ: OPEN>READ>CLOSE of FILE %s\n", file);

							if( errAPI( "OPEN", openFile(file, 0, trashdir), curr, file, &sleeptime) ) break;
							if( errAPI( "READ", readFile(file, &readbuf, &readsize), curr, file, &sleeptime) ) break;

							ErrNEG1(  SAVEfile( readbuf, readsize, file, readdir)  );	if(PRINT) printf("\t %zuB READ", readsize);
							
							if( errAPI( "CLOSE", closeFile(file), curr, file, &sleeptime) ) break;

							break;
						
						case 'l':
							if(PRINT) printf("REQ: OPEN>LOCK of FILE %s\n", file);

							if( errAPI( "OPEN", openFile(file, 0, trashdir), curr, file, &sleeptime) ) break;	
							if( errAPI( "LOCK", lockFile(file), curr, file, &sleeptime) ) break;

							break;
							
						case 'u':
							if(PRINT) printf("REQ: UNLOCK>CLOSE of FILE %s\n", file);

							if( errAPI( "UNLOCK", unlockFile(file), curr, file, &sleeptime) ) break;
							if( errAPI( "CLOSE", closeFile(file), curr, file, &sleeptime) ) break;

							break;
						
						case 'c':
							if(PRINT) printf("REQ: REMOVAL of FILE %s\n", file);

							if( errAPI( "REMOVE", removeFile(file), curr, file, &sleeptime) ) break;

							break;
						}
					if(PRINT) printf("\n");
					if(PRINT) fflush(stdout);
					}
				} break;
				
				
			}
		free(curr);		//Destroys the dequeued OPT NODE
		}
		
	ErrNEG1(  closeConnection(socketname)  );
	
	ErrNEG1(  optQueueDestroy()  );
	
	return 0;

ErrCLEANUP
	if(optQueue) optQueueDestroy();
	return -1;
ErrCLEAN
  }
}
	
/*
\tDefault behaviour: all files in the dir will be sent to the FSS\n

						if(n_w<0){
							fprintf(stderr,"Error: -w option requires n=0 or no n to send all files in a dir, or n!=0 to send at most n files\n");
							ErrFAIL;
							}

fprintf(stderr,"Errorr: -w error while opening specified dir, %s\n", strerror(errno));
*/
