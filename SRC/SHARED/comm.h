
#include <linux/limits.h>	//PATH_MAX

typedef enum CmdCode{
	IDLE    ,
	OPEN	,
	CLOSE	,
	WRITE	,
	APPEND	,
	READ	,
	READN	,
	LOCK	,
	UNLOCK	,
	REMOVE	,
	QUIT
	} CmdCode;
	
char* strCmdCode(CmdCode cmd){
	switch(cmd){
		case IDLE: 	return "IDLE";
		case OPEN: 	return "OPEN";
		case CLOSE: return "CLOSE";
		case WRITE: return "WRITE";
		case APPEND:return "APPEND";
		case READ: 	return "READ";
		case READN: return "READN";
		case LOCK:	return "LOCK";
		case UNLOCK:return "UNLOCK";
		case REMOVE:return "REMOVE";
		case QUIT: 	return "QUIT";
		}
	return NULL;
	}

typedef enum Reply{
	OK		   ,
	ANOTHER    ,
	CACHE      ,
	NOTFOUND   ,
	EXISTS	   ,
	LOCKED	   ,
	NOTOPEN	   ,
	ALROPEN	   ,
	NOTLOCKED  ,
	ALRLOCKED  ,
	EMPTY	   ,
	NOTEMPTY   ,
	TOOBIG
	} Reply;

char* strReply(Reply reply){
	switch(reply){
		case OK: 		return "OK";
		case ANOTHER: 	return "ANOTHER";
		case CACHE: 	return "CACHE";
		case NOTFOUND: 	return "NOTFOUND";
		case EXISTS: 	return "EXISTS";
		case LOCKED: 	return "LOCKED";
		case NOTOPEN: 	return "NOTOPEN";
		case ALROPEN: 	return "ALROPEN";
		case NOTLOCKED:	return "NOTLOCKED";
		case ALRLOCKED: return "ALRLOCKED";
		case EMPTY: 	return "EMPTY";
		case NOTEMPTY: 	return "NOTEMPTY";
		case TOOBIG: 	return "TOOBIG";
		}
	return NULL;
	}	
	
#define	O_CREATE 0x1
#define O_LOCK	 0x2

typedef struct {
	CmdCode code;
	int info;		//'open' flags   or   number of files in readn/cache alg
	char filename[PATH_MAX];
	} Cmd;
