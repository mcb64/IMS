#undef DEBUG
#define USE_TYPED_RSET
#define VERSION 2.0

#include <algorithm>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <alarm.h>
#include <dbDefs.h>
#include <callback.h>
#include <dbStaticLib.h>
#include <dbAccess.h>
#include <dbScan.h>
#include <recGbl.h>
#include <recSup.h>
#include <dbEvent.h>
#include <devSup.h>
#include <math.h>
#include <time.h>
#include <errlog.h>
#include <unistd.h>

#define GEN_SIZE_OFFSET
#include "imsRecord.h"
#undef GEN_SIZE_OFFSET

#include "drvAsynIPPort.h"
#include "asynDriver.h"
#include "asynOctet.h"
#include "asynOctetSyncIO.h"

#include "epicsExport.h"
#include "epicsMessageQueue.h"

#include "ims.h"


/*** Forward references ***/


/*** Record Support Entry Table (RSET) functions ***/

static long init_record       ( dbCommon *precord, int pass                );
static long process           ( dbCommon *precord                          );
static long special           ( dbAddr   *pDbAddr, int after               );
static long cvt_dbaddr        ( dbAddr   *pDbAddr                          );
static long get_units         ( dbAddr   *pDbAddr, char *                  );
static long get_precision     ( const dbAddr   *pDbAddr, long *            );
static long get_graphic_double( dbAddr   *pDbAddr, struct dbr_grDouble   * );
static long get_control_double( dbAddr   *pDbAddr, struct dbr_ctrlDouble * );
static long get_alarm_double  ( dbAddr   *pDbAddr, struct dbr_alDouble   * );


rset imsRSET =
{
    RSETNUMBER,
    NULL,
    NULL,
    init_record,
    process,
    special,
    NULL,
    cvt_dbaddr,
    NULL,
    NULL,
    get_units,
    get_precision,
    NULL,
    NULL,
    NULL,
    get_graphic_double,
    get_control_double,
    get_alarm_double
};

extern "C" { epicsExportAddress( rset, imsRSET ); }


#define MIP_DONE     0x0000    // No motion is in progress
#define MIP_MOVE     0x0001    // A move not resulting from Jog* or Hom*
#define MIP_RETRY    0x0002    // A retry is in progress
#define MIP_NEW      0x0004    // Stop current move for a new move
#define MIP_BL       0x0008    // Done moving, now take out backlash
#define MIP_HOMF     0x0010    // A home-forward command is in progress
#define MIP_HOMR     0x0020    // A home-reverse command is in progress
#define MIP_HOMB     0x0040    // Back off from the limit switch
#define MIP_HOMC     0x0080    // Creep to the limit switch
#define MIP_HOME     (MIP_HOMF | MIP_HOMR)
#define MIP_JOGF     0x0100    // Jog forward
#define MIP_JOGR     0x0200    // Jog backward
#define MIP_JOG      (MIP_JOGF | MIP_JOGR)
#define MIP_TRIG     0x0400    // Triggered move
#define MIP_CALI     0x0800    // Calibration
#define MIP_PAUSE    0x1000    // Move is being paused
#define MIP_STOP     0x2000    // We're trying to stop.  If a home command
                               // is issued when the motor is moving, we
                               // stop the motor first

#define MAX_MSG_SIZE 61

#define OK            0


static long connect_motor     ( imsRecord *precord                            );
static long init_motor        ( imsRecord *precord                            );
static long process_motor_info( imsRecord *precord, status_word csr,  
                                                    long count                );
static void new_move          ( imsRecord *precord                            );
static void post_fields       ( imsRecord *precord, unsigned short alarm_mask,
                                                    unsigned short all        );

static void enforce_S         ( imsRecord *precord                            );
static void enforce_BS        ( imsRecord *precord                            );
static void enforce_HS        ( imsRecord *precord                            );

static long send_msg          ( struct ims_info *mInfo, char const *msg       );
static long send_recv_reply   ( struct ims_info *mInfo, char const *msg,
				char *rbbuf                                   );
static void ping_controller   ( struct ims_info *mInfo                        );
static void io_controller     ( struct ims_info *mInfo                        );
static void pf_controller     ( struct ims_info *mInfo                        );

static long log_msg  ( imsRecord *prec, int dlvl, const char *fmt, ... );
static void post_msgs( imsRecord *prec                                 );


/*** Debugging ***/

volatile int imsRecordDebug = 0;

extern "C" { epicsExportAddress( int, imsRecordDebug ); }

using namespace std;


/******************************************************************************/
static long init_record( dbCommon *precord, int pass )
{
    imsRecord *prec = (imsRecord *)precord;
    ims_info  *mInfo;

    long       status = 0;
    int        i;
    char       buf[512];

    if ( pass > 0 ) { return( status ); }

    mInfo = (ims_info *)malloc( sizeof(ims_info) );
    mInfo->precord   = prec;
    mInfo->pEvent    = new epicsEvent( epicsEventEmpty );
    mInfo->sEvent    = new epicsEvent( epicsEventFull  );
    mInfo->cMutex    = new epicsMutex();
    mInfo->rMutex    = new epicsMutex();
    mInfo->pfq       = new epicsMessageQueue(20, sizeof(struct pf_asyn_req));

    /* Organize the requests into a linked list. */
    for (i = 0; i < REQ_CNT; i++) {
	mInfo->req[i].state = IAR_EMPTY;
	mInfo->req[i].link = (i==REQ_CNT-1) ? -1 : (i+1);
	mInfo->req[i].rbEvent = new epicsEvent();
    }
    mInfo->reqH = 0;

    mInfo->lMutex    = new epicsMutex();
    mInfo->mLength   = 61;
    mInfo->cIndex    = 0;
    mInfo->newMsg    = 0;
    mInfo->saving    = 0;
    mInfo->sAddr     = (char *)calloc( 8*61, sizeof(char) );

    sprintf(buf, "%s.SSTR", prec->name);
    mInfo->sstr_status = dbNameToAddr(buf, &mInfo->sstr_addr);
    sprintf(buf, "%s.SVNG", prec->name);
    mInfo->svng_status = dbNameToAddr(buf, &mInfo->svng_addr);
    sprintf(buf, "%s:SAVED_STR.VAL", prec->name);
    mInfo->saved_status = dbNameToAddr(buf, &mInfo->saved_addr);

    prec->loga       = mInfo->sAddr;
    prec->logb       = mInfo->sAddr + mInfo->mLength * 1;
    prec->logc       = mInfo->sAddr + mInfo->mLength * 2;
    prec->logd       = mInfo->sAddr + mInfo->mLength * 3;
    prec->loge       = mInfo->sAddr + mInfo->mLength * 4;
    prec->logf       = mInfo->sAddr + mInfo->mLength * 5;
    prec->logg       = mInfo->sAddr + mInfo->mLength * 6;
    prec->logh       = mInfo->sAddr + mInfo->mLength * 7;

    prec->dpvt       = mInfo;

    gethostname( prec->host, 60                   );
    strcpy     ( prec->iocn, getenv("EPICS_NAME") );

    connect_motor ( prec );
    init_motor    ( prec );

    if ( (prec->smax > 0.) && (prec->smax < prec->sbas) )
    {
        prec->sbas = prec->smax;
        db_post_events( prec, &prec->sbas, DBE_VAL_LOG );
    }

    prec->vbas = prec->urev * prec->sbas;
    prec->vmax = prec->urev * prec->smax;

    enforce_S ( prec );
    enforce_BS( prec );
    enforce_HS( prec );

    if ( prec->dhlm < prec->dllm ) prec->dhlm = prec->dllm;

    if ( prec->dir == imsDIR_Pos )
    {
        prec->llm  = prec->off + prec->dllm;
        prec->hlm  = prec->off + prec->dhlm;
    }
    else
    {
        prec->llm  = prec->off - prec->dhlm;
        prec->hlm  = prec->off - prec->dllm;
    }

    if ( (prec->val < prec->llm) || (prec->val > prec->hlm) ) prec->lvio = 1;
    else                                                      prec->lvio = 0;

    return( status );
}

/******************************************************************************/
static long connect_motor( imsRecord *prec )
{
    ims_info          *mInfo = (ims_info *)prec->dpvt;
    asynUser          *pasynUser;
    char               serialPort[80];
    static const char  output_terminator[] = "\n";
    static const char  input_terminator[]  = "\r\n";
    motor_status       msta;
    asynStatus         asyn_rtn;
    int                status = 0;

    msta.All = 0;

    sprintf( serialPort, "%s TCP", prec->port );

    mInfo->cMutex->lock();

    drvAsynIPPortConfigure              ( prec->asyn, serialPort, 0, 0, 0 );
    asyn_rtn = pasynOctetSyncIO->connect( prec->asyn, 0, &pasynUser, NULL );
#ifdef DEBUG
    /* Debugging from the beginning of time. */
    pasynTrace->setTraceIOMask(pasynUser, 2);
    pasynTrace->setTraceMask(pasynUser, 9);
#endif

    if ( asyn_rtn != asynSuccess )
    {
        log_msg( prec, 0, "Failed to connect to the port" );

        msta.Bits.RA_PROBLEM = 1;
    }

    /* We will manually attach the output terminator. */
    pasynOctetSyncIO->setOutputEos( pasynUser, output_terminator,
                                    strlen(output_terminator)     );
    pasynOctetSyncIO->setInputEos ( pasynUser, input_terminator,
                                    strlen(input_terminator)      );

    mInfo->pasynUser = pasynUser;

    mInfo->cMutex->unlock();

    epicsThreadCreate( prec->name, epicsThreadPriorityMedium,
                       epicsThreadGetStackSize(epicsThreadStackMedium),
                       (EPICSTHREADFUNC)io_controller, (void *)mInfo );

    epicsThreadCreate( prec->name, epicsThreadPriorityMedium,
                       epicsThreadGetStackSize(epicsThreadStackMedium),
                       (EPICSTHREADFUNC)pf_controller, (void *)mInfo );

    epicsThreadCreate( prec->name, epicsThreadPriorityMedium,
                       epicsThreadGetStackSize(epicsThreadStackMedium),
                       (EPICSTHREADFUNC)ping_controller, (void *)mInfo );

    prec->msta = msta.All;
    return( status );
}

/******************************************************************************/
static long init_motor( imsRecord *prec )
{
    ims_info       *mInfo = (ims_info *)prec->dpvt;

    char            msg[MAX_MSG_SIZE], rbbuf[MAX_MSG_SIZE];
    int             s1, s2, s3, s4, s9, a1, a2, a3, a4, a9, d1, d2, d3, d4, d9;
    int             rbve, rbby, retry, VI, VM, A, PT, status = OK;
    epicsUInt32     old_msta = prec->msta;
    motor_status    msta;

    unsigned short  alarm_mask;

    mInfo->cMutex->lock();

    msta.All = prec->msta; /* This might have been set in connect_motor! */

    // read the part number
    retry = 1;
    do
    {
        status = send_recv_reply( mInfo, "PR \"PN=\",PN", rbbuf );
        if ( (status > 0) && (strlen(rbbuf) > 3) )
        {
            if      ( strncmp(rbbuf,                   "PN=", 3) == 0 )
                strncpy( prec->pn, rbbuf+3, 61              );
            else if ( strncmp(rbbuf+(strlen(rbbuf)-3), "PN=", 3) == 0 )
                strncpy( prec->pn, rbbuf,   strlen(rbbuf)-3 );
            else
                status = 0;
        }
        else
            status = 0;

        epicsThreadSleep( 0.2 );
    } while ( (status <= 0) && (retry++ < 3) );

    if ( status <= 0 )
    {
        log_msg( prec, 0, "Failed to read the part number" );

        msta.Bits.RA_PROBLEM = 1;
        goto finished;
    }

    // read the serial number
    retry = 1;
    do
    {
        status = send_recv_reply( mInfo, "PR \"SN=\",SN", rbbuf );
        if ( (status > 0) && (strlen(rbbuf) > 3) )
        {
            if      ( strncmp(rbbuf,                   "SN=", 3) == 0 )
                strncpy( prec->sn, rbbuf+3, 61              );
            else if ( strncmp(rbbuf+(strlen(rbbuf)-3), "SN=", 3) == 0 )
                strncpy( prec->sn, rbbuf,   strlen(rbbuf)-3 );
            else
                status = 0;
        }
        else
            status = 0;

        epicsThreadSleep( 0.2 );
    } while ( (status <= 0) && (retry++ < 3) );

    if ( status <= 0 )
    {
        log_msg( prec, 0, "Failed to read the serial number" );

        msta.Bits.RA_PROBLEM = 1;
        goto finished;
    }

    // read the firmware version
    retry = 1;
    do
    {
        status = send_recv_reply( mInfo, "PR \"VR=\",VR", rbbuf );
        if ( (status > 0) && (strlen(rbbuf) > 3) )
        {
            if      ( strncmp(rbbuf,                   "VR=", 3) == 0 )
                strncpy( prec->vr, rbbuf+3, 61              );
            else if ( strncmp(rbbuf+(strlen(rbbuf)-3), "VR=", 3) == 0 )
                strncpy( prec->vr, rbbuf,   strlen(rbbuf)-3 );
            else
                status = 0;
        }
        else
            status = 0;

        epicsThreadSleep( 0.2 );
    } while ( (status <= 0) && (retry++ < 3) );

    if ( status <= 0 )
    {
        log_msg( prec, 0, "Failed to read the firmware version" );

        msta.Bits.RA_PROBLEM = 1;
        goto finished;
    }

    // read S1 - S4
    retry = 1;
    do
    {
        status = send_recv_reply( mInfo, 
				  "PR \"S1=\",S1,\", S2=\",S2,\", S3=\",S3,\", S4=\",S4",
				  rbbuf );
        if ( status > 0 )
        {
            status = sscanf( rbbuf, "S1=%d, %d, %d, S2=%d, %d, %d, S3=%d, %d, %d, S4=%d, %d, %d",
                                    &s1, &a1, &d1, &s2, &a2, &d2, &s3, &a3, &d3, &s4, &a4, &d4 );
            if ( status == 12 )
            {
                if      ( (s1 ==  0) && (a1 == 0) && (d1 == 0) )
                    prec->s1 = imsS19_NotUsed;
                else if ( (s1 ==  2) && (a1 == 0) && (d1 == 0) )
                    prec->s1 = imsS19_LMTpL  ;
                else if ( (s1 ==  2) && (a1 == 1) && (d1 == 0) )
                    prec->s1 = imsS19_LMTpH  ;
                else if ( (s1 ==  3) && (a1 == 0) && (d1 == 0) )
                    prec->s1 = imsS19_LMTmL  ;
                else if ( (s1 ==  3) && (a1 == 1) && (d1 == 0) )
                    prec->s1 = imsS19_LMTmH  ;
                else
                {
                    log_msg( prec, 0, "S1 setting invalid" );

                    prec->s1             = imsS19_Invalid;
                    msta.Bits.RA_PROBLEM = 1;
                }

                if      ( (s2 ==  0) && (a2 == 0) && (d2 == 0) )
                    prec->s2 = imsS19_NotUsed;
                else if ( (s2 ==  2) && (a2 == 0) && (d2 == 0) )
                    prec->s2 = imsS19_LMTpL  ;
                else if ( (s2 ==  2) && (a2 == 1) && (d2 == 0) )
                    prec->s2 = imsS19_LMTpH  ;
                else if ( (s2 ==  3) && (a2 == 0) && (d2 == 0) )
                    prec->s2 = imsS19_LMTmL  ;
                else if ( (s2 ==  3) && (a2 == 1) && (d2 == 0) )
                    prec->s2 = imsS19_LMTmH  ;
                else
                {
                    log_msg( prec, 0, "S2 setting invalid" );

                    prec->s2             = imsS19_Invalid;
                    msta.Bits.RA_PROBLEM = 1;
                }

                if      ( (s3 ==  0) && (a3 == 0) && (d3 == 0) )
                    prec->s3 = imsS19_NotUsed;
                else if ( (s3 == 16) && (a3 == 1) && (d3 == 1) )
                    prec->s3 = imsS19_5VOut  ;
                else
                {
                    log_msg( prec, 0, "S3 setting invalid" );

                    prec->s3             = imsS19_Invalid;
                    msta.Bits.RA_PROBLEM = 1;
                }

                if      ( (s4 ==  0) && (a4 == 0) && (d4 == 0) )
                    prec->s4 = imsS19_NotUsed;
                else if ( (s4 ==  1) && (a4 == 0) && (d4 == 0) )
                    prec->s4 = imsS19_HomeL  ;
                else if ( (s4 ==  1) && (a4 == 1) && (d4 == 0) )
                    prec->s4 = imsS19_HomeH  ;
                else if ( (s4 == 17) && (a4 == 0) && (d4 == 0) )
                    prec->s4 = imsS19_Brake  ;
                else
                {
                    log_msg( prec, 0, "S4 setting invalid" );

                    prec->s4             = imsS19_Invalid;
                    msta.Bits.RA_PROBLEM = 1;
                }

                db_post_events( prec, &prec->s1  , DBE_VAL_LOG );
                db_post_events( prec, &prec->s2  , DBE_VAL_LOG );
                db_post_events( prec, &prec->s3  , DBE_VAL_LOG );
                db_post_events( prec, &prec->s4  , DBE_VAL_LOG );
            }
        }

        epicsThreadSleep( 0.2 );
    } while ( (status != 12) && (retry++ < 3) );

    if ( status != 12 )
    {
        log_msg( prec, 0, "Failed to read S1 - S4" );

        msta.Bits.RA_PROBLEM = 1;
        goto finished;
    }

    // read S9
    if ( (strncmp(prec->pn, "MFI", 3) != 0) && (strstr(prec->pn, "-EE") == 0) )
    {
        retry = 1;
        do
        {
            status = send_recv_reply( mInfo, "PR \"S9=\",S9", rbbuf );
            if ( status > 0 )
            {
                status = sscanf( rbbuf, "S9=%d, %d, %d", &s9, &a9, &d9 );
                if ( status == 3 )
                {
                    if      ( (s9 ==  0) && (a9 == 0) && (d9 == 0) )
                        prec->s9 = imsS19_NotUsed;
                    else if ( (s9 == 17) && (a9 == 0) && (d9 == 0) )
                        prec->s9 = imsS19_Brake  ;
                    else
                    {
                        log_msg( prec, 0, "S9 setting invalid" );

                        prec->s9             = imsS19_Invalid;
                        msta.Bits.RA_PROBLEM = 1;
                    }

                    db_post_events( prec, &prec->s9  , DBE_VAL_LOG );
                }
            }

            epicsThreadSleep( 0.2 );
        } while ( (status != 3) && (retry++ < 3) );

        if ( status != 3 )
        {
            log_msg( prec, 0, "Failed to read S9" );

            prec->s9 = imsS19_NotUsed;
            db_post_events( prec, &prec->s9  , DBE_VAL_LOG );
        }
    }
    else
    {
        prec->s9 = imsS19_NotUsed;
        db_post_events( prec, &prec->s9  , DBE_VAL_LOG );
    }

    // read MS and ES
    retry = 1;
    do
    {
        status = send_recv_reply( mInfo, "PR \"MS=\",MS,\", ES=\",ES", rbbuf );
        if ( status > 0 )
        {
            status = sscanf( rbbuf, "MS=%d, ES=%d", &s1, &s2 );
            if ( (status == 2) && (s2 >= 0) && (s2 <= 3) )
            {
                prec->ms = s1;
                prec->es = s2;

                db_post_events( prec, &prec->ms  , DBE_VAL_LOG );
                db_post_events( prec, &prec->es  , DBE_VAL_LOG );
            }
            else
                status = 0;
        }

        epicsThreadSleep( 0.2 );
    } while ( (status != 2) && (retry++ < 3) );

    if ( status != 2 )
    {
        log_msg( prec, 0, "Failed to read MS and ES" );

        msta.Bits.RA_PROBLEM = 1;
        goto finished;
    }

    // check the MCode version and running status
    retry = 1;
    do
    {
        status = send_recv_reply( mInfo, "PR \"VE=\",VE,\", BY=\",BY", rbbuf );
        if ( status > 0 )
        {
            status = sscanf( rbbuf, "VE=%d, BY=%d", &rbve, &rbby );
            if ( (status != 2) || (rbby != 0 && rbby != 1) ) status = 0;
        }

        epicsThreadSleep( 0.2 );
    } while ( (status != 2) && (retry++ < 3) );

    if ( status != 2 )
    {
        log_msg( prec, 0, "Failed to read the MCode version and BY" );

        rbve = -999;
        rbby =    0;
    }

    if ( rbve != prec->dver )
    {
        char line[256];

        log_msg( prec, 0, "Load MCode ..." );
        post_msgs( prec );

        send_msg( mInfo, "E"      );
        epicsThreadSleep( 1 );

        send_msg( mInfo, "CP"     );
        epicsThreadSleep( 1 );

        send_msg( mInfo, "CP 0,1" );
        epicsThreadSleep( 1 );

        strcpy( line, getenv("IMS") );
        strcat( line, "/misc/"      );
        strcat( line, prec->mpgm    );

        FILE *fp = fopen( line, "r" );
        while( 1 )
        {
            fgets( line, 64, fp );
            if ( ferror(fp) || feof(fp) ) break;

            send_msg( mInfo, line );
            log_msg( prec, -1, line );

            epicsThreadSleep( 0.1 );
        }

        fclose(fp);
        epicsThreadSleep( 2 );

        rbby = 0;
    }

    if ( rbby == 0 )
    {
        send_msg( mInfo, "p1 P" );
        epicsThreadSleep( 0.1 );

        retry = 1;
        do
        {
            log_msg( prec, 0, "MCode not running, start it ..." );

            send_msg( mInfo, "EX 1" );
            epicsThreadSleep( 1 );

            status = send_recv_reply( mInfo, "PR \"BY=\",BY", rbbuf );
            if ( status > 0 )
            {
                status = sscanf( rbbuf, "BY=%d", &rbby );
                if ( (status == 1) && (rbby == 1) ) break;
            }

            epicsThreadSleep( 0.2 );
        } while ( retry++ < 3 );

        if ( (status == 1) && (rbby == 1) )
        {
            send_msg( mInfo, "PU 0" );
            epicsThreadSleep( 0.1 );

            send_msg( mInfo, "SV 9" );
            epicsThreadSleep( 1   );
        }
        else
        {
            log_msg( prec, 0, "Failed to start MCode" );

            msta.Bits.RA_PROBLEM = 1;
            goto finished;
        }
    }

    // set the parameters
    send_msg( mInfo, "NE 0" );

    epicsThreadSleep( 0.1 );

    sprintf( msg, "LM %d\r\nSM %d\r\nSF %d", prec->lm, prec->sm, prec->sf   );
    send_msg( mInfo, msg );

    epicsThreadSleep( 0.1 );

    sprintf( msg, "EE %d\r\nEL %d\r\nMT %d", prec->ee, int(prec->el+0.5),
                                                                 prec->mt   );
    send_msg( mInfo, msg );

    epicsThreadSleep( 0.1 );

    sprintf( msg, "RC %d\r\nHC %d\r\nHT %d", prec->rc, prec->hc, prec->ht   );
    send_msg( mInfo, msg );

    epicsThreadSleep( 0.1 );

    sprintf( msg, "Me %d\r\nMm %d",          prec->me, prec->mode           );
    send_msg( mInfo, msg );

    if ( prec->mode == imsMode_TrigMR )
    {
        epicsThreadSleep( 0.1 );

        VI = NINT( prec->vbas / prec->res );
        VM = NINT( prec->velo / prec->res );
        A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );
        PT = NINT( (1. - 2.*prec->dir) * prec->twv / prec->res );

        sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nPt %d\r\nTe 4",
                      VI, VM, A, PT );

        send_msg( mInfo, msg );

        log_msg( prec, 0, "Waiting for trigger ..." );
    }

    if ( prec->mres == 0. )
    {
        prec->srev = prec->frev * prec->ms  ;
        prec->mres = prec->urev / prec->srev;
        db_post_events( prec, &prec->srev, DBE_VAL_LOG );
        db_post_events( prec, &prec->mres, DBE_VAL_LOG );
    }
    else
    {
        if ( int(prec->urev/prec->mres + 0.000001) != (prec->frev * prec->ms) )
        {
            log_msg( prec, 0, "SREV != FREV*MS, please check" );

            msta.Bits.RA_PROBLEM = 1;
        }
    }

    if ( prec->eres == 0. )
    {
        prec->eres = prec->urev / prec->el / 4;
        db_post_events( prec, &prec->eres, DBE_VAL_LOG );
    }
    else
    {
        if ( fabs(prec->urev/prec->el/4/prec->eres - 1) > 0.01 )
        {
            log_msg( prec, 0, "ERES != UREV/EL/4, please check" );

            msta.Bits.RA_PROBLEM = 1;
        }
    }

    if ( prec->ee == imsAble_Enable ) prec->res = prec->eres;
    else                              prec->res = prec->mres;
    db_post_events( prec, &prec->res,  DBE_VAL_LOG );

    if ( prec->hc > 0 )
    {
        prec->hcsv = prec->hc;
        prec->hctg = imsHC_Restore;
        db_post_events( prec, &prec->hcsv, DBE_VAL_LOG );
        db_post_events( prec, &prec->hctg, DBE_VAL_LOG );
    }
    else
    {
        prec->hctg = imsHC_Zero;
        db_post_events( prec, &prec->hctg, DBE_VAL_LOG );
    }

    mInfo->initialized = 0;
    msta.Bits.NOT_INIT = 1;

    prec->dmov = 1;
    prec->mip  = MIP_DONE;

    // let controller send a status update every ~5 seconds until first process
    send_msg( mInfo, "Us 300" );

    finished:
    mInfo->cMutex->unlock();

    if      ( msta.Bits.RA_PROBLEM )                                 // hardware
        recGblSetSevr( (dbCommon *)prec, COMM_ALARM,  INVALID_ALARM );
    else if ( msta.Bits.NOT_INIT   )                                 // NOT_INIT
    {
        recGblSetSevr( (dbCommon *)prec, STATE_ALARM, MAJOR_ALARM   );

        log_msg( prec, 0, "Wait for first status update" );
    }

    prec->msta = msta.All;

    if ( old_msta != prec->msta ) MARK( M_MSTA );

    recGblGetTimeStamp( prec );

    alarm_mask = recGblResetAlarms( prec );
    post_fields( prec, alarm_mask, 1 );
    post_msgs  ( prec                );

    return( 0 );
}

/******************************************************************************/
static long process( dbCommon *precord )
{
    imsRecord      *prec = (imsRecord *)precord;
    ims_info       *mInfo = (ims_info *)prec->dpvt;
    char            msg[MAX_MSG_SIZE], old_mstr[61];
    unsigned short  old_mip,  alarm_mask;
    short           old_dmov, old_rcnt, old_miss;
    double          old_val,  old_dval, old_diff, diff;
    long            count, old_rrbv, old_rval, nval, status = OK;
    int             VI, VM, A;
    status_word     csr;
    motor_status    old_msta, msta;

    bool            first = FALSE, reset_us = FALSE;

    if ( prec->pact ) return( OK );

    prec->pact = 1;
    log_msg( prec, 1, "Process" );

    old_mip      = prec->mip ;
    old_dmov     = prec->dmov;
    old_rcnt     = prec->rcnt;
    old_miss     = prec->miss;
    old_val      = prec->val ;
    old_dval     = prec->dval;
    old_rval     = prec->rval;

    old_msta.All = prec->msta;

    strncpy( old_mstr, prec->mstr, 61 );

    mInfo->cMutex->lock();
    csr.All = mInfo->csr;
    if ( (mInfo->newData != 1) && (mInfo->newData != 2) )
    {
        mInfo->cMutex->unlock();
        goto exit;
    }
    else if ( mInfo->newData == 2 )                        // callback from ping
    {
        msta.All = prec->msta | mInfo->csr;

        mInfo->newData = 0;
        mInfo->cMutex->unlock();

        goto finished;
    }

    csr.All = mInfo->csr;
    count   = mInfo->count;
    first   = ! mInfo->initialized;

    if ( first )
    {
        mInfo->initialized = 1;
        log_msg( prec, 0, "Initialization completed" );

        reset_us = TRUE;
    }

    mInfo->newData = 0;
    mInfo->cMutex->unlock();

    old_rrbv = prec->rrbv;

    process_motor_info( prec, csr, count );

    msta.All = prec->msta;

    if ( prec->movn )                                                  // moving
    {
        if ( prec->mip & MIP_TRIG )
        {
            prec->mip &= ~MIP_TRIG;
            log_msg( prec, 0, "Triggered" );
        }
        else if ( (prec->mode == imsMode_TrigMR) && (prec->dmov == 1) )
        {
            prec->smov = 1;
            prec->dmov = 0;
            prec->mip  = MIP_MOVE;
            log_msg( prec, 0, "Triggered" );
        }
        else if ( (prec->jogr != 0) && (prec->rbv <= prec->llm) )
        {
            log_msg( prec, 0, "Reached low limit, stop jogging ..." );
            prec->mip &= ~MIP_PAUSE;
            prec->mip |=  MIP_STOP ;

            send_msg( mInfo, "SL 0\r\nUs 0" );
        }
        else if ( (prec->jogf != 0) && (prec->rbv >= prec->hlm) )
        {
            log_msg( prec, 0, "Reached high limit, stop jogging ..." );
            prec->mip &= ~MIP_PAUSE;
            prec->mip |=  MIP_STOP ;

            send_msg( mInfo, "SL 0\r\nUs 0" );
        }

        goto finished;
    }
    else if ( msta.Bits.RA_TE )                           // waiting for trigger
    {
        if ( (prec->mode != imsMode_TrigMR) || (prec->dmov == 1) )
            goto finished;
    }

    if ( (prec->smov ==       1) && (prec->dmov           == 0) &&
         (prec->ocsr == csr.All) && (labs(old_rrbv-count) <  5)    )
    {
        prec->smov = 0;
        goto finished;                                           // stale status
    }

    prec->smov = 0;

    diff = prec->rbv  - prec->val;
    if ( prec->mip == MIP_DONE )             // was not moving, check slip_stall
    {
        old_diff   = prec->diff;
        prec->diff = diff;

        if ( fabs(prec->diff) > prec->pdbd )
        {
            log_msg( prec, 0, "Slipped, diff = %.6g", prec->diff );
            msta.Bits.EA_SLIP_STALL = 1;
        }

        if ( old_diff != prec->diff ) MARK( M_DIFF );

        goto finished;
    }

    if ( (prec->mode == imsMode_TrigMR) ||                      // delta trigger
         (msta.Bits.RA_STALL && (! msta.Bits.RA_SM)) ||               // stalled
         prec->lls || prec->hls ||                         // hit a limit switch
         (prec->mip & MIP_HOME) ||                                 // was homing
         (prec->mip & MIP_CALI) ||                            // was calibrating
         (prec->mip & (MIP_PAUSE | MIP_STOP))           )       // pause or stop
    {
        short newval = 1;

        if      ( prec->mode == imsMode_TrigMR )                // delta trigger
        {
            log_msg( prec, 0, "Moved to %.6g", prec->rbv );
            log_msg( prec, 0, "Waiting for trigger ..."  );
        }
        else if ( msta.Bits.RA_STALL && (! msta.Bits.RA_SM) )
        {
            if ( prec->mip & MIP_JOG )                            // was jogging
            {
                prec->jogf = 0;
                prec->jogr = 0;
                log_msg( prec, 0, "Jogging stopped due to stall" );
            }
            else if ( prec->mip & MIP_HOME )                       // was homing
            {
                if ( prec->htyp == imsHTYP_Stall )
                {
                    prec->athm         = 1;
                    msta.Bits.RA_HOMED = 1;
                    msta.Bits.RA_HOME  = 1;
                    log_msg( prec, 0, "Homed to stall/hard stop" );

                    db_post_events( prec, &prec->athm, DBE_VAL_LOG );
                }
                else
                {
                    prec->miss = 1;
                    log_msg( prec, 0, "Stalled, missed home" );
                }
            }
            else if ( prec->mip & MIP_CALI )                  // was calibrating
            {
                log_msg( prec, 0, "Stalled, failed to calibrate" );
            }
            else if ( (prec->mip != MIP_DONE) &&
                      !(prec->mip & (MIP_PAUSE | MIP_STOP)) )      // was moving
            {
                if ( fabs(diff) < prec->rdbd )
                {
                    log_msg( prec, 0, "Desired %.6g, reached %.6g",
                                      prec->val, prec->rbv );
                }
                else
                {
                    prec->miss = 1;
                    log_msg( prec, 0, "Desired %.6g, reached %.6g, missed due to ST",
                                      prec->val, prec->rbv );
                }
            }
        }
        else if ( prec->lls )
        {
            if ( prec->mip & MIP_JOG )                            // was jogging
            {
                prec->jogf = 0;
                prec->jogr = 0;
                log_msg( prec, 0, "Jogging stopped due to LLS" );
            }
            else if ( (prec->htyp == imsHTYP_Limits       ) &&
                      (prec->mip  == MIP_HOMR             )    )  // home to LLS
            {
                log_msg( prec, 0, "Back off from LLS (HDST: %.6g), with ACCL & VELO",
                                  prec->hdst );

                prec->smov  =        1;
                prec->mip  |= MIP_HOMB;
                if ( prec->dir == imsDIR_Pos )
                    nval = NINT(       prec->hdst / prec->res );
                else
                    nval = NINT( -1. * prec->hdst / prec->res );

                VI = NINT( prec->vbas / prec->res );
                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );
                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nR2 0\r\nMR %ld",
                              VI, VM, A, nval );

                send_msg( mInfo, msg    );
//              send_msg( mInfo, "Us 0" );

                newval = 0;
            }
            else if ( (prec->htyp == imsHTYP_Limits       ) &&
                      (prec->mip  == (MIP_HOMR | MIP_HOMC))    ) // creep to LLS
            {
                log_msg( prec, 0, "Homed to LLS" );

                prec->athm = 1;
                msta.Bits.RA_HOMED = 1;
                msta.Bits.RA_HOME  = 1;

                db_post_events( prec, &prec->athm, DBE_VAL_LOG );
            }
            else if ( prec->mip & MIP_HOME )                       // was homing
            {
                prec->miss = 1;
                log_msg( prec, 0, "Hit low limit, missed home" );
            }
            else if ( prec->mip & MIP_CALI )                  // was calibrating
            {
                log_msg( prec, 0, "Hit low limit, failed to calibrate" );
            }
            else if ( (prec->mip != MIP_DONE) &&
                      !(prec->mip & (MIP_PAUSE | MIP_STOP)) )      // was moving
            {
                if ( fabs(diff) < prec->rdbd )
                {
                    log_msg( prec, 0, "Desired %.6g, reached %.6g",
                                      prec->val, prec->rbv );
                }
                else
                {
                    prec->miss = 1;
                    log_msg( prec, 0, "Desired %.6g, reached %.6g, missed due to LLS",
                                      prec->val, prec->rbv );
                }
            }
        }
        else if ( prec->hls )
        {
            if ( prec->mip & MIP_JOG )                            // was jogging
            {
                prec->jogf = 0;
                prec->jogr = 0;
                log_msg( prec, 0, "Jogging stopped due to HLS" );
            }
            else if ( (prec->htyp == imsHTYP_Limits       ) &&
                      (prec->mip  == MIP_HOMF             )    )  // home to HLS
            {
                log_msg( prec, 0, "Back off from HLS (HDST: %.6g), with ACCL & VELO",
                                  prec->hdst );

                prec->smov  =        1;
                prec->mip  |= MIP_HOMB;
                if ( prec->dir == imsDIR_Pos )
                    nval = NINT( -1. * prec->hdst / prec->res );
                else
                    nval = NINT(       prec->hdst / prec->res );

                VI = NINT( prec->vbas / prec->res );
                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );
                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nR2 0\r\nMR %ld",
                              VI, VM, A, nval );

                send_msg( mInfo, msg    );
//              send_msg( mInfo, "Us 0" );

                newval = 0;
            }
            else if ( (prec->htyp == imsHTYP_Limits       ) &&
                      (prec->mip  == (MIP_HOMF | MIP_HOMC))    ) // creep to HLS
            {
                log_msg( prec, 0, "Homed to HLS" );

                prec->athm = 1;
                msta.Bits.RA_HOMED = 1;
                msta.Bits.RA_HOME  = 1;

                db_post_events( prec, &prec->athm, DBE_VAL_LOG );
            }
            else if ( prec->mip & MIP_HOME )                       // was homing
            {
                prec->miss = 1;
                log_msg( prec, 0, "Hit high limit, missed home" );
            }
            else if ( prec->mip & MIP_CALI )                  // was calibrating
            {
                log_msg( prec, 0, "Hit high limit, failed to calibrate" );
            }
            else if ( (prec->mip != MIP_DONE) &&
                      !(prec->mip & (MIP_PAUSE | MIP_STOP)) )      // was moving
            {
                if ( fabs(diff) < prec->rdbd )
                {
                    log_msg( prec, 0, "Desired %.6g, reached %.6g",
                                      prec->val, prec->rbv );
                }
                else
                {
                    prec->miss = 1;
                    log_msg( prec, 0, "Desired %.6g, reached %.6g, missed due to HLS",
                                      prec->val, prec->rbv );
                }
            }
        }
        else if ( prec->mip & MIP_PAUSE )
        {
            log_msg( prec, 0, "Paused at %.6g", prec->rbv  );
            newval = 0;
        }
        else if ( prec->mip & MIP_STOP  )
        {
            log_msg( prec, 0, "Stopped at %.6g", prec->rbv );

            if ( prec->mip & MIP_JOG )                            // was jogging
            {
                prec->jogf = 0;
                prec->jogr = 0;
            }
        }
        else if ( prec->mip == (MIP_HOMR | MIP_HOMB) )
        {
            log_msg( prec, 0, "Creep to LLS, with HACC & HVEL" );

            prec->smov = 1;
            prec->mip  = MIP_HOMR | MIP_HOMC;

            VI = NINT( prec->vbas / prec->res );
            VM = NINT( prec->hvel / prec->res );
            A  = NINT( (prec->hvel - prec->vbas) / prec->res / prec->hacc );
            if ( prec->dir == imsDIR_Pos )
                sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nR2 0\r\nSL -%d", VI, A, VM );
            else
                sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nR2 0\r\nSL  %d", VI, A, VM );

            send_msg( mInfo, msg    );
//          send_msg( mInfo, "Us 0" );

            newval = 0;
        }
        else if ( prec->mip == (MIP_HOMF | MIP_HOMB) )
        {
            log_msg( prec, 0, "Creep to HLS, with HACC & HVEL" );

            prec->smov = 1;
            prec->mip  = MIP_HOMF | MIP_HOMC;

            VI = NINT( prec->vbas / prec->res );
            VM = NINT( prec->hvel / prec->res );
            A  = NINT( (prec->hvel - prec->vbas) / prec->res / prec->hacc );
            if ( prec->dir == imsDIR_Pos )
                sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nR2 0\r\nSL  %d", VI, A, VM );
            else
                sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nR2 0\r\nSL -%d", VI, A, VM );

            send_msg( mInfo, msg    );
//          send_msg( mInfo, "Us 0" );

            newval = 0;
        }
        else if ( prec->mip & MIP_HOME  )
        {
            prec->athm = 1;
            msta.Bits.RA_HOMED = 1;
            if ( prec->htyp == imsHTYP_Switch )
            {
                log_msg( prec, 0, "Homed to home switch"  );
                msta.Bits.RA_HOME = 1;
            }
            else
            {
                log_msg( prec, 0, "Homed to encoder mark" );
                msta.Bits.EA_HOME = 1;
            }

            db_post_events( prec, &prec->athm, DBE_VAL_LOG );
        }
        else if ( prec->mip & MIP_CALI )                      // was calibrating
        {
            log_msg( prec, 0, "Calibration finished" );
        }

        if ( newval )
        {
            prec->mip  = MIP_DONE;
            prec->dmov = 1;
            prec->val  = prec->rbv;
            prec->dval = prec->drbv;
            prec->rval = prec->rrbv;
            prec->diff = 0;
        }

        prec->rcnt = 0;

        reset_us   = TRUE;
    }
    else if ( prec->mip == MIP_NEW ) new_move( prec );
    else if ( prec->mip == MIP_BL  )                              // do backlash
    {
        // check for miss
        if ( (prec->ee == imsAble_Disable) && (prec->me == imsRBV_Encoder) &&
             (fabs(prec->dval - prec->bdst -
                   (prec->rbv - prec->off)*(1-2*prec->dir)) >= prec->rdbd)    )
        {
            prec->miss = 1;
            log_msg( prec, 0, "Backlash correction may not be reliable !" );
        }

        log_msg( prec, 0, "Move to %.6g (DVAL: %.6g), with BACC & BVEL",
                          prec->val, prec->dval );

        prec->smov = 1;
        prec->mip  = MIP_MOVE;
        prec->rval = NINT(prec->dval / prec->res);

        VI = NINT( prec->vbas / prec->res );
        VM = NINT( prec->bvel / prec->res );
        A  = NINT( (prec->bvel - prec->vbas) / prec->res / prec->bacc );
        sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nR2 0\r\nMA %d",
                      VI, VM, A, prec->rval );
        send_msg( mInfo, msg    );
//      send_msg( mInfo, "Us 0" );
    }
//  else if ( (imsMode_Normal   == prec->mode) &&       // normal mode, not scan
    else if ( (fabs(diff) >= prec->rdbd) &&                 // not closed enough
              (prec->rcnt <  prec->rtry) &&                    // can retry more
              (((prec->egag == menuYesNoYES) && (msta.Bits.RA_COMM_ERR == 0)) ||
               ((prec->egag == menuYesNoNO ) &&
                ((prec->ee  == imsAble_Enable  ) ||
                 (prec->res >= fabs(prec->bdst))    ) ) ) )
    {
        prec->smov  = 1;
        prec->mip  |= MIP_RETRY;

        log_msg( prec, 0, "Desired %.6g, reached %.6g, retrying %d ...",
                          prec->val,  prec->rbv,  ++prec->rcnt );

        if      ( prec->egag == menuYesNoYES   )
            sprintf( msg, "MR %d", int((prec->dval - prec->drbv) / prec->res) );
        else if ( prec->ee   == imsAble_Enable )
            sprintf( msg, "MA %d", prec->rval                                 );
        else if ( prec->dir  == imsDIR_Pos     )              // monitor encoder
            sprintf( msg, "MR %d", int((prec->val  - prec->rbv ) / prec->res) );
        else if ( prec->dir  == imsDIR_Neg     )              // monitor encoder
            sprintf( msg, "MR %d", int((prec->rbv  - prec->val ) / prec->res) );

        send_msg( mInfo, msg    );
//      send_msg( mInfo, "Us 0" );
    }
    else          // finished backlash, close enough, or no (more) retry allowed
    {
        prec->diff = diff;
        if ( fabs(diff) < prec->rdbd )
            log_msg( prec, 0, "Desired %.6g, reached %.6g",
                              prec->val, prec->rbv             );
        else
        {
            prec->miss = 1;
            log_msg( prec, 0, "Desired %.6g, reached %.6g after %d retries",
                              prec->val, prec->rbv, prec->rcnt );
        }

        prec->mip  = MIP_DONE;
        prec->dmov = 1;
        prec->rcnt = 0;

        reset_us   = TRUE;
    }

    finished:
    if ( old_mip  != prec->mip  ) MARK( M_MIP  );
    if ( old_dmov != prec->dmov ) MARK( M_DMOV );
    if ( old_rcnt != prec->rcnt ) MARK( M_RCNT );
    if ( old_miss != prec->miss ) MARK( M_MISS );
    if ( old_val  != prec->val  ) MARK( M_VAL  );
    if ( old_dval != prec->dval ) MARK( M_DVAL );
    if ( old_rval != prec->rval ) MARK( M_RVAL );

    prec->ocsr = csr.All;

    if ( reset_us ) send_msg( mInfo, "Us 18000" );

    if      ( msta.Bits.RA_PROBLEM                                )  // hardware
    {
        recGblSetSevr( (dbCommon *)prec, COMM_ALARM,  INVALID_ALARM );

        sprintf( prec->mstr, "HW Problem"       );
        log_msg( prec, 0,    "Hardware problem" );
    }
    else if ( msta.Bits.NOT_INIT   || msta.Bits.RA_BY0         ||//NOT_INIT,BY=0
              msta.Bits.RA_NE      || (prec->lls && prec->hls)    )      // NE=1
    {
        recGblSetSevr( (dbCommon *)prec, STATE_ALARM, MAJOR_ALARM   );

        if      ( msta.Bits.NOT_INIT     )
        {
            sprintf( prec->mstr, "Not Initialized"             );
            log_msg( prec, 0,    "Initialization not complete" );
        }
        else if ( msta.Bits.RA_BY0       )
        {
            sprintf( prec->mstr, "MCode Stopped"               );
            log_msg( prec, 0,    "MCode not running"           );
        }
        else if ( msta.Bits.RA_NE        )
        {
            sprintf( prec->mstr, "NE = 1"                      );
            log_msg( prec, 0,    "Numeric Enable is set"       );
        }
        else if ( prec->lls && prec->hls )
        {
            sprintf( prec->mstr, "Both Limits Hit"             );
            log_msg( prec, 0,    "Both limits hit"             );
        }
    }
    else if ( prec->disp || msta.Bits.RA_POWERUP ||
              (msta.Bits.RA_STALL && (! msta.Bits.RA_SM)) ||
              msta.Bits.EA_SLIP_STALL                             )
    {
        recGblSetSevr( (dbCommon *)prec, STATE_ALARM, MINOR_ALARM   );

        if      ( prec->disp                                )
        {
            sprintf( prec->mstr, "Disabled"     );
            log_msg( prec, 0,    "Disabled"     );
        }
        else if ( msta.Bits.RA_POWERUP                      )
        {
            sprintf( prec->mstr, "Power Cycled" );
            log_msg( prec, 0,    "Power cycled" );
        }
        else if ( msta.Bits.RA_STALL && (! msta.Bits.RA_SM) )
        {
            sprintf( prec->mstr, "Stalled"      );
            log_msg( prec, 0,    "Stalled"      );
        }
        else if ( msta.Bits.EA_SLIP_STALL                   )
        {
            sprintf( prec->mstr, "Slip / Stall" );
            log_msg( prec, 0,    "Slip / Stall" );
        }
    }
    else
    {
        if ( prec->dmov == 0 )
        {
            if ( msta.Bits.RA_STALL )                          // stall detected
            {
                if ( prec->stsv > NO_ALARM )
                    recGblSetSevr( (dbCommon *)prec, STATE_ALARM, prec->stsv );

                sprintf( prec->mstr, "Moving/Stalling" );
                log_msg( prec, 0,    "Stall detected"  );
            }
            else
                sprintf( prec->mstr, "Moving"          );
        }
        else
        {
            if      ( prec->lvio                )
                sprintf( prec->mstr, "Limit Violated"         );
            else if ( prec->spg == imsSPG_Pause )
                sprintf( prec->mstr, "Paused"                 );
            else if ( prec->spg == imsSPG_Stop  )
                sprintf( prec->mstr, "Stopped"                );
            else if ( msta.Bits.RA_STALL        )              // stall detected
                sprintf( prec->mstr, "Ready / Stall Detected" );
            else if ( msta.Bits.RA_ERR          )                   // got error
                sprintf( prec->mstr, "Ready / Error"          );
            else
                sprintf( prec->mstr, "Ready"                  );

            if ( msta.Bits.RA_STALL )
            {
                if ( prec->stsv > NO_ALARM )
                    recGblSetSevr( (dbCommon *)prec, STATE_ALARM, prec->stsv );

                log_msg( prec, 0, "Stall detected"                 );
            }

            if ( msta.Bits.RA_ERR   )
            {
                if ( prec->ersv > NO_ALARM )
                    recGblSetSevr( (dbCommon *)prec, STATE_ALARM, prec->ersv );

                log_msg( prec, 0, "Got error %d", msta.Bits.RA_ERR );
            }

            if ( ! first )
            {
                if      ( old_msta.Bits.RA_POWERUP && ! msta.Bits.RA_POWERUP   )
                    log_msg( prec, 0, "Power-cycle bit cleared" );
                else if ( old_msta.Bits.RA_STALL   && ! msta.Bits.RA_STALL     )
                    log_msg( prec, 0, "Stall bit cleared"       );
                else if ( old_msta.Bits.RA_ERR     && ! msta.Bits.RA_ERR       )
                    log_msg( prec, 0, "Error cleared"           );
                else if ( (prec->sevr > NO_ALARM)  && (prec->nsev == NO_ALARM) )
                                                    // had alarm/warnings before
                    log_msg( prec, 0, "Alarm/warnings cleared" );
            }
        }
    }

    msta.Bits.RA_DONE   = prec->dmov;
    msta.Bits.RA_MOVING = prec->movn;

    prec->msta          = msta.All;
    if ( prec->msta != old_msta.All        ) MARK( M_MSTA );

    if ( strcmp(prec->mstr, old_mstr) != 0 ) MARK( M_MSTR );

    recGblGetTimeStamp( prec );

    alarm_mask = recGblResetAlarms( prec );
    post_fields( prec, alarm_mask, 0 );
    post_msgs  ( prec                );

    recGblFwdLink     ( prec );                 // process the forward scan link

    exit:
    prec->proc = 0;
    prec->pact = 0;

    return( status );
}

/******************************************************************************/
static long process_motor_info( imsRecord *prec, status_word csr, long count )
{
    ims_info     *mInfo = (ims_info *)prec->dpvt;
    short         old_movn, old_rlls, old_rhls, old_lls, old_hls;
    double        old_drbv, old_rbv;
    long          old_rrbv, status = 0;

    motor_status  msta;

    int           dir = (prec->dir == imsDIR_Pos) ? 1 : -1;

    if ( csr.Bits.BY0 )
    {
        msta.All         = prec->msta;
        msta.Bits.RA_BY0 = csr.Bits.BY0;
        prec->msta       = msta.All;
        prec->movn       = 0;

        return( status );
    }

    old_movn   = prec->movn;
    old_rlls   = prec->rlls;
    old_rhls   = prec->rhls;
    old_lls    = prec->lls ;
    old_hls    = prec->hls ;
    old_rrbv   = prec->rrbv;
    old_drbv   = prec->drbv;
    old_rbv    = prec->rbv ;

    msta.All             = 0;

    msta.Bits.RA_MOVING  = csr.Bits.MOVING;
    msta.Bits.RA_EE      = csr.Bits.EE    ;
    msta.Bits.RA_SM      = csr.Bits.SM    ;
    msta.Bits.RA_STALL   = csr.Bits.ST    ;
    msta.Bits.RA_TE      = csr.Bits.TE    ;
    msta.Bits.RA_POWERUP = csr.Bits.PU    ;
    msta.Bits.RA_NE      = csr.Bits.NE    ;
    msta.Bits.RA_ERR     = csr.Bits.ERR & 127;

    if ( (msta.Bits.RA_ERR == 83) || (msta.Bits.RA_ERR == 84) )
        msta.Bits.RA_ERR = 0;

    prec->movn = msta.Bits.RA_MOVING;

    prec->rlls = 0;
    prec->rhls = 0;
    if ( (prec->s1 == imsS19_LMTmL) || (prec->s1 == imsS19_LMTmH) )
        prec->rlls = csr.Bits.I1;
    if ( (prec->s2 == imsS19_LMTmL) || (prec->s2 == imsS19_LMTmH) )
        prec->rlls = csr.Bits.I2;

    if ( (prec->s1 == imsS19_LMTpL) || (prec->s1 == imsS19_LMTpH) )
        prec->rhls = csr.Bits.I1;
    if ( (prec->s2 == imsS19_LMTpL) || (prec->s2 == imsS19_LMTpH) )
        prec->rhls = csr.Bits.I2;

    msta.Bits.RA_MINUS_LS = prec->rlls;
    msta.Bits.RA_PLUS_LS  = prec->rhls;

    prec->lls  = (dir == 1) ? prec->rlls : prec->rhls;
    prec->hls  = (dir == 1) ? prec->rhls : prec->rlls;

    prec->rrbv = count;
    if ( prec->egag == menuYesNoNO )
    {
        if ( (prec->ee == imsAble_Disable) && (prec->me == imsRBV_Encoder) )
        {
            prec->rrbv = prec->rval;
            prec->drbv = prec->dval;
            prec->rbv  = dir * count * prec->eres + prec->off;
        }
        else
        {
            prec->drbv = prec->rrbv * prec->res;
            prec->rbv  = dir * prec->drbv + prec->off;
        }
    }
    else
    {
        status = ! dbIsLinkConnected( &prec->erbl );
        if ( status == 0 )
        {
            status = dbGetLink( &prec->erbl, DBR_DOUBLE, &prec->erbv, 0, 0 );
            if ( status != 0 )
            {
                log_msg( prec, 0, "Failed to read the external gauge" );
                msta.Bits.RA_COMM_ERR = 1;
            }
            else if ( prec->nsta == LINK_ALARM )
            {
                log_msg( prec, 0, "External gauge has problem" );
                msta.Bits.RA_COMM_ERR = 1;
            }
            else
            {
                prec->drbv = prec->erbv * prec->eskl;
                prec->rbv  = dir * prec->drbv + prec->off;

                db_post_events( prec, &prec->erbv, DBE_VAL_LOG );
            }
        }
        else
        {
            log_msg( prec, 0, "External gauge not connected" );
            msta.Bits.RA_COMM_ERR = 1;
        }

        if ( msta.Bits.RA_COMM_ERR && prec->movn )
        {
            prec->mip &= ~MIP_PAUSE;
            prec->mip |=  MIP_STOP ;

            send_msg( mInfo, "SL 0\r\nUs 0" );

            log_msg( prec, 0, "Stop the motion"  );
        }
    }

    prec->msta = msta.All;

    if ( old_movn != prec->movn) MARK( M_MOVN );
    if ( old_rlls != prec->rlls) MARK( M_RLLS );
    if ( old_rhls != prec->rhls) MARK( M_RHLS );
    if ( old_lls  != prec->lls ) MARK( M_LLS  );
    if ( old_hls  != prec->hls ) MARK( M_HLS  );
    if ( old_rrbv != prec->rrbv) MARK( M_RRBV );
    if ( old_drbv != prec->drbv) MARK( M_DRBV );
    if ( old_rbv  != prec->rbv ) MARK( M_RBV  );

    return( status );
}

/******************************************************************************/
static void new_move( imsRecord *prec )
{
    ims_info *mInfo = (ims_info *)prec->dpvt;
    char      msg[MAX_MSG_SIZE];
    int       VI, VM, A;

    if ( (prec->egag == menuYesNoNO) && (fabs(prec->bdst) > prec->res) )
    {                                                                // backlash
        if ( ((prec->bdst > 0) && (prec->drbv > prec->dval))    ||
             ((prec->bdst < 0) && (prec->drbv < prec->dval))    ||
             (fabs(prec->drbv - prec->dval) > fabs(prec->bdst))    )
        {           // opposite direction, or long move, use ACCL and VELO first
            log_msg( prec, 0, "Move to %.6g (DVAL: %.6g), with ACCL & VELO",
                              prec->val, prec->dval-prec->bdst );

            VI = NINT( prec->vbas / prec->res );
            VM = NINT( prec->velo / prec->res );
            A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );

            prec->mip  = MIP_BL  ;
            prec->rval = NINT((prec->dval - prec->bdst) / prec->res);
            if ( prec->mode == imsMode_TrigMA )
                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nPt %d\r\nTe 4",
                              VI, VM, A, prec->rval );
            else
                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nMA %d",
                              VI, VM, A, prec->rval );
        }
        else                // same direction and within BDST, use BACC and BVEL
        {
            log_msg( prec, 0, "Move to %.6g (DVAL: %.6g), with BACC & BVEL",
                              prec->val, prec->dval            );

            VI = NINT( prec->vbas / prec->res );
            VM = NINT( prec->bvel / prec->res );
            A  = NINT( (prec->bvel - prec->vbas) / prec->res / prec->bacc );

            prec->mip  = MIP_MOVE;
            prec->rval = NINT(prec->dval                / prec->res);
            if ( prec->mode == imsMode_TrigMA )
                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nPt %d\r\nTe 4",
                              VI, VM, A, prec->rval );
            else
                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nMA %d",
                              VI, VM, A, prec->rval );
        }
    }
    else                                       // no backlash, use ACCL and VELO
    {
        log_msg( prec, 0, "Move to %.6g (DVAL: %.6g), with ACCL & VELO",
                          prec->val, prec->dval );

        VI = NINT( prec->vbas / prec->res );
        VM = NINT( prec->velo / prec->res );
        A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );

        prec->mip  = MIP_MOVE;
        if ( prec->egag == menuYesNoNO )
            prec->rval = NINT(prec->dval / prec->res);
        else
            prec->rval = prec->rrbv + NINT((prec->dval-prec->drbv) / prec->res);

        if ( prec->mode == imsMode_TrigMA )
            sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nPt %d\r\nTe 4",
                          VI, VM, A, prec->rval );
        else
            sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nMA %d",
                          VI, VM, A, prec->rval );
    }

    if ( prec->mode == imsMode_TrigMA ) prec->mip |= MIP_TRIG;

    prec->smov = 1;
    prec->dmov = 0;
    send_msg( mInfo, msg );

    if ( prec->mode == imsMode_TrigMA )
        log_msg( prec, 0, "Waiting for trigger ..." );
    else
        send_msg( mInfo, "R2 0\r\nUs 30" );

    return;
}

/******************************************************************************/
static long special( dbAddr *pDbAddr, int after )
{
    imsRecord      *prec = (imsRecord *) pDbAddr->precord;
    ims_info       *mInfo = (ims_info *)prec->dpvt;
    char            MI = (prec->htyp == imsHTYP_Switch) ? 'M' : 'I';
    char            msg[MAX_MSG_SIZE], rbbuf[MAX_MSG_SIZE];
    long            csr, count, old_rval, new_c1, new_c2;
    short           old_dmov, old_rcnt, old_miss, old_lvio;
    double          nval, old_val, old_dval, old_rbv, new_dval;
    unsigned short  old_mip, alarm_mask = 0;
    motor_status    msta;

    int             dir = (prec->dir == imsDIR_Pos) ? 1 : -1;
    int             VI, VM, A, PT, fieldIndex = dbGetFieldIndex( pDbAddr );
    int             status = OK;

    if ( after == 0 )
    {
        if      ( fieldIndex == imsRecordVAL  ) prec->lval = prec->val ;
        else if ( fieldIndex == imsRecordDVAL ) prec->oval = prec->dval;
        else if ( fieldIndex == imsRecordSPG  ) prec->oval = prec->spg ;
        else if ( fieldIndex == imsRecordDIR  ) prec->oval = prec->dir ;
        else if ( fieldIndex == imsRecordOFF  ) prec->oval = prec->off ;
        else if ( fieldIndex == imsRecordSET  ) prec->oval = prec->set ;
        else if ( fieldIndex == imsRecordLLM  ) prec->oval = prec->llm ;
        else if ( fieldIndex == imsRecordHLM  ) prec->oval = prec->hlm ;
        else if ( fieldIndex == imsRecordDLLM ) prec->oval = prec->dllm;
        else if ( fieldIndex == imsRecordDHLM ) prec->oval = prec->dhlm;
        else if ( fieldIndex == imsRecordEL   ) prec->oval = prec->el  ;
        else if ( fieldIndex == imsRecordMS   ) prec->oval = prec->ms  ;
        else if ( fieldIndex == imsRecordUREV ) prec->oval = prec->urev;
        else if ( fieldIndex == imsRecordFREV ) prec->oval = prec->frev;
        else if ( fieldIndex == imsRecordSREV ) prec->oval = prec->srev;
        else if ( fieldIndex == imsRecordEE   ) prec->oval = prec->ee  ;
        else if ( fieldIndex == imsRecordRCMX ) prec->oval = prec->rcmx;
        else if ( fieldIndex == imsRecordRC   ) prec->oval = prec->rc  ;
        else if ( fieldIndex == imsRecordHCMX ) prec->oval = prec->hcmx;
        else if ( fieldIndex == imsRecordHC   ) prec->oval = prec->hc  ;
        else if ( fieldIndex == imsRecordMODE ) prec->oval = prec->mode;

        return( OK );
    }

    old_val  = prec->val ;
    old_dval = prec->dval;
    old_rval = prec->rval;
    old_rbv  = prec->rbv ;
    old_mip  = prec->mip ;
    old_dmov = prec->dmov;
    old_rcnt = prec->rcnt;
    old_miss = prec->miss;
    old_lvio = prec->lvio;

    msta.All = prec->msta;

    switch( fieldIndex )
    {
        case imsRecordPING:
            prec->ping = 0;
            if ( prec->mode != imsMode_Scan ) mInfo->pEvent->signal();

            break;
        case imsRecordSSTR:
            log_msg( prec, 1, "%s", prec->sstr  );

            status = sscanf( prec->sstr, "%ld,P=%ldEOS", &csr, &count );
            if ( status != 2 )
            status = sscanf( prec->sstr, "%ldP,=%ldEOS", &csr, &count );

            if ( status == 2 )
            {
                log_msg( prec, 2, "CSR=%d,P=%d", csr, count );

                mInfo->cMutex->lock();

                mInfo->csr     = csr;
                mInfo->count   = count;
                mInfo->newData = 1;

                mInfo->cMutex->unlock();

                dbScanLock  ( (dbCommon *)prec );
                process     ( (dbCommon *)prec );
                dbScanUnlock( (dbCommon *)prec );
            }
            else
                log_msg( prec, 0, "Erroneous status string" );

            break;
        case imsRecordSVNG:
            if      ( strcmp(prec->svng, "Save")        == 0 )
            {
		if (mInfo->saving) {
		    log_msg( prec, 0, "Double save request, aborting!" );

		    mInfo->saving = 0;
		    mInfo->sEvent->signal();
		} else {
		    log_msg( prec, 0, "Saving ..." );

		    mInfo->sEvent->wait();
		    mInfo->saving = 1;
		    send_msg( mInfo, "SV 9" );
		}
            }
            else if ( sscanf(prec->svng, "%ld", &count) == 1 )
            {
		if (mInfo->saving) {
		    mInfo->saving = 0;
		    mInfo->sEvent->signal();
		}

                new_dval = count * prec->res;
                nval     = dir * new_dval + prec->off;
                log_msg( prec, 0, "Saved %.6g (dial: %.6g, count: %d)",
                                  nval, new_dval, count );
            }

            break;
        case imsRecordVAL :
            MARK( M_VAL  );

            change_val:
            if ( (prec->sevr >  MINOR_ALARM) ||
                 msta.Bits.RA_POWERUP || msta.Bits.RA_COMM_ERR ||
                 ((prec->set == imsSET_Use) && (prec->spg != imsSPG_Go)) )
            {
                prec->val  = prec->lval;

                if      ( msta.Bits.RA_PROBLEM   )
                    log_msg( prec, 0, "No move/set, hardware problem"  );
                else if ( msta.Bits.RA_NE        )
                    log_msg( prec, 0, "No move/set, NE is set"         );
                else if ( msta.Bits.RA_BY0       )
                    log_msg( prec, 0, "No move/set, MCode not running" );
                else if ( msta.Bits.NOT_INIT     )
                    log_msg( prec, 0, "No move/set, init not finished" );
                else if ( msta.Bits.RA_POWERUP   )
                    log_msg( prec, 0, "No move/set, power cycled"      );
                else if ( msta.Bits.RA_COMM_ERR  )
                    log_msg( prec, 0, "No move/set, ext gauge problem" );
                else if ( prec->spg != imsSPG_Go )
                    log_msg( prec, 0, "No move, SPG is not Go"         );
                else
                    log_msg( prec, 0, "No move/set, unknown alarm"     );

                break;
            }

            if ( prec->set == imsSET_Set )                            // no move
            {
                log_msg( prec, 0, "Set position from %.6g to %.6g", prec->rbv,
                                                                    prec->val );

                prec->rbv  = prec->val;
                if ( (prec->foff == imsOFF_Variable) ||
                     (prec->egag == menuYesNoYES   )    )
                {
                    nval       = prec->off;

                    prec->off  = prec->rbv - prec->drbv * (1. - 2.*prec->dir);
                    db_post_events( prec, &prec->off,  DBE_VAL_LOG );

                    if ( prec->dir == imsDIR_Pos )
                    {
                        prec->llm  = prec->off + prec->dllm;
                        prec->hlm  = prec->off + prec->dhlm;
                    }
                    else
                    {
                        prec->llm  = prec->off - prec->dhlm;
                        prec->hlm  = prec->off - prec->dllm;
                    }

                    db_post_events( prec, &prec->llm,  DBE_VAL_LOG );
                    db_post_events( prec, &prec->hlm,  DBE_VAL_LOG );

                    log_msg( prec, 0, "Changed OFF from %.6g to %.6g",
                                      nval, prec->off );
                }
                else
                {
                    prec->dval = (prec->rbv - prec->off) * (1. - 2.*prec->dir);
                    prec->rval = NINT(prec->dval / prec->res);

                    if ( prec->ee == imsAble_Enable )
                    {
                        log_msg( prec, 0, "Changed C2 from %ld to %ld",
                                          prec->rrbv, prec->rval );

                        new_c2 = prec->rval;
                        new_c1 = NINT( prec->dval / prec->mres );
                    }
                    else
                    {
                        log_msg( prec, 0, "Changed C1 from %ld to %ld",
                                          prec->rrbv, prec->rval );

                        new_c1 = prec->rval;
                        new_c2 = NINT( prec->dval / prec->eres );
                    }
                    sprintf( msg, "C1 %ld\r\nC2 %ld\r\nUs 0", new_c1, new_c2 );

                    send_msg( mInfo, msg );
                }

                break;
            }

            new_dval = (prec->val - prec->off) * (1. - 2.*prec->dir);
            if ( (prec->val < prec->llm) || (prec->val > prec->hlm) ||
                 ((prec->bdst > 0) && (prec->drbv > new_dval) &&
                  ((new_dval - prec->bdst) < prec->dllm)         ) ||
                 ((prec->bdst < 0) && (prec->drbv < new_dval) &&
                  ((new_dval - prec->bdst) > prec->dhlm)         )     )
            {                                    // violated the software limits
                prec->lvio = 1;                     // set limit violation alarm
                prec->val  = prec->lval;

                log_msg( prec, 0, "No move, limit violated" );
                break;
            }

            prec->dval = new_dval;
            goto do_move;
        case imsRecordDVAL:
            MARK( M_DVAL );

            if ( (prec->sevr >  MINOR_ALARM) ||
                 msta.Bits.RA_POWERUP || msta.Bits.RA_COMM_ERR ||
                 ((prec->set == imsSET_Use) && (prec->spg != imsSPG_Go)) )
            {
                prec->dval = prec->oval;

                if      ( msta.Bits.RA_PROBLEM   )
                    log_msg( prec, 0, "No move/set, hardware problem"  );
                else if ( msta.Bits.RA_NE        )
                    log_msg( prec, 0, "No move/set, NE is set"         );
                else if ( msta.Bits.RA_BY0       )
                    log_msg( prec, 0, "No move/set, MCode not running" );
                else if ( msta.Bits.NOT_INIT     )
                    log_msg( prec, 0, "No move/set, init not finished" );
                else if ( msta.Bits.RA_POWERUP   )
                    log_msg( prec, 0, "No move/set, power cycled"      );
                else if ( msta.Bits.RA_COMM_ERR  )
                    log_msg( prec, 0, "No move/set, ext gauge problem" );
                else if ( prec->spg != imsSPG_Go )
                    log_msg( prec, 0, "No move, SPG is not Go"         );
                else
                    log_msg( prec, 0, "No move/set, unknown alarm"     );

                break;
            }

            if ( prec->set == imsSET_Set )                            // no move
            {
                log_msg( prec, 0, "Set dail from %.6g to %.6g", prec->drbv,
                                                                prec->dval );

                prec->rval = NINT(prec->dval / prec->res);
                prec->val  = prec->dval * (1. - 2.*prec->dir) + prec->off;

                if ( prec->ee == imsAble_Enable )
                {
                    log_msg( prec, 0, "Changed C2 from %ld to %ld",
                                      prec->rrbv, prec->rval );

                    new_c2 = prec->rval;
                    new_c1 = NINT( prec->dval / prec->mres );
                }
                else
                {
                    log_msg( prec, 0, "Changed C1 from %ld to %ld",
                                      prec->rrbv, prec->rval );

                    new_c1 = prec->rval;
                    new_c2 = NINT( prec->dval / prec->eres );
                }
                sprintf( msg, "C1 %ld\r\nC2 %ld\r\nUs 0", new_c1, new_c2 );

                send_msg( mInfo, msg );

                break;
            }

            if ( (prec->dval < prec->dllm) || (prec->dval > prec->dhlm) ||
                 ((prec->bdst > 0) && (prec->drbv > prec->dval) &&
                  ((prec->dval - prec->bdst) < prec->dllm)         )    ||
                 ((prec->bdst < 0) && (prec->drbv < prec->dval) &&
                  ((prec->dval - prec->bdst) > prec->dhlm)         )       )
            {                                    // violated the hardware limits
                prec->lvio = 1;                     // set limit violation alarm
                prec->dval = prec->oval;

                log_msg( prec, 0, "No move, limit violated" );
                break;
            }

            prec->lval = prec->val;
            prec->val  = prec->dval * (1. - 2.*prec->dir) + prec->off;

            do_move:
            prec->lvio = 0;
            prec->rcnt = 0;
            prec->miss = 0;

            prec->athm = 0;
            msta.Bits.RA_HOMED = 0;
            msta.Bits.RA_HOME  = 0;
            msta.Bits.EA_HOME  = 0;

            db_post_events( prec, &prec->athm, DBE_VAL_LOG );

            // if ( prec->spg != imsSPG_Go  ) break;

            if ( prec->dmov == 0 )                               // still moving
            {
                if ( prec->mip != MIP_NEW )           // stop current move first
                {
                    log_msg( prec, 0, "Stop current move" );
                    prec->mip  = MIP_NEW;

                    send_msg( mInfo, "SL 0\r\nUs 0" );
                }

                break;
            }

            new_move( prec );

            break;
        case imsRecordTWV :
            if ( prec->mode == imsMode_TrigMR )
            {
                VI = NINT( prec->vbas / prec->res );
                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );
                PT = NINT( (1. - 2.*prec->dir) * prec->twv / prec->res );

                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nPt %d",
                              VI, VM, A, PT );

                send_msg( mInfo, msg );
            }

            break;
        case imsRecordTWF :
            nval = prec->rbv + prec->twf * prec->twv;
            goto tweak;
        case imsRecordTWR :
            nval = prec->rbv - prec->twr * prec->twv;

            tweak:
            prec->lval = prec->val;
            prec->val  = nval;

            goto change_val;
        case imsRecordSPG :
            if ( (prec->spg == prec->oval) || (prec->mip == MIP_DONE) ) break;

            if ( prec->spg == imsSPG_Go )
            {
                log_msg( prec, 0, "Resume moving" );
                prec->mip &= ~( MIP_PAUSE | MIP_STOP );

                sprintf( msg, "MA %d", prec->rval );
                send_msg( mInfo, msg );

                break;
            }
            else
            {
                if ( prec->spg == imsSPG_Stop )
                {
                    if ( prec->mip & MIP_TRIG )
                    {
                        send_msg( mInfo, "TE 0" );
                        log_msg( prec, 0, "Disable trigger"          );
                    }
                    else
                        log_msg( prec, 0, "Stop, with deceleration"  );

                    prec->mip &= ~MIP_PAUSE;
                    prec->mip |=  MIP_STOP ;
                }
                else
                {
                    log_msg( prec, 0, "Pause, with deceleration" );
                    prec->mip |=  MIP_PAUSE;
                }

                send_msg( mInfo, "SL 0\r\nUs 0" );
            }

            break;
        case ( imsRecordSTOP ):
            prec->stop = 0;

            if ( prec->mip != MIP_DONE )
            {
                if ( prec->mip & MIP_TRIG )
                {
                    send_msg( mInfo, "TE 0" );
                    log_msg( prec, 0, "Disable trigger"          );
                }
                else
                    log_msg( prec, 0, "Stop, with deceleration"  );

                prec->mip  &= ~MIP_PAUSE;
                prec->mip  |=  MIP_STOP ;

                send_msg( mInfo, "SL 0\r\nUs 0" );
            }

            break;
        case ( imsRecordESTP ):
            prec->estp = 0;

            if ( prec->mip & MIP_TRIG ) send_msg( mInfo, "TE 0\r\n\e" );
            else                        send_msg( mInfo, "\e"         );

            if ( prec->mip != MIP_DONE )
            {
                prec->mip  &= ~MIP_PAUSE;
                prec->mip  |=  MIP_STOP ;
                prec->spg   = imsSPG_Stop;
                db_post_events( prec, &prec->spg,  DBE_VAL_LOG );

                log_msg( prec, 0, "Emergency stop !!!" );
            }
            else
                log_msg( prec, 0, "Reset controller"   );

            // force a status update
            send_msg( mInfo, "PR \"BOS65536,P=\",P,\"EOS\"" );

            break;
        case ( imsRecordJOGF ):
        case ( imsRecordJOGR ):
            if ( (prec->sevr >  MINOR_ALARM) ||
                 msta.Bits.RA_POWERUP || msta.Bits.RA_COMM_ERR ||
                 ((prec->set == imsSET_Use) && (prec->spg != imsSPG_Go)) )
            {
                prec->jogf = 0;
                prec->jogr = 0;

                if      ( msta.Bits.RA_PROBLEM   )
                    log_msg( prec, 0, "No jogging, hardware problem"  );
                else if ( msta.Bits.RA_NE        )
                    log_msg( prec, 0, "No jogging, NE is set"         );
                else if ( msta.Bits.RA_BY0       )
                    log_msg( prec, 0, "No jogging, MCode not running" );
                else if ( msta.Bits.NOT_INIT     )
                    log_msg( prec, 0, "No jogging, init not finished" );
                else if ( msta.Bits.RA_POWERUP   )
                    log_msg( prec, 0, "No jogging, power cycled"      );
                else if ( msta.Bits.RA_COMM_ERR  )
                    log_msg( prec, 0, "No jogging, ext gauge problem" );
                else if ( prec->spg != imsSPG_Go )
                    log_msg( prec, 0, "No jogging, SPG is not Go"     );
                else
                    log_msg( prec, 0, "No jogging, unknown alarm"     );

                break;
            }
            else if ( ((prec->jogf != 0) && (prec->rbv >= prec->hlm)) || 
                      ((prec->jogr != 0) && (prec->rbv <= prec->llm))    )
            {
                prec->jogf = 0;
                prec->jogr = 0;

                log_msg( prec, 0, "No jogging, limit violated" );

                break;
            }

            if ( (prec->jogf == 0) && (prec->jogr == 0) )
            {
                if ( prec->mip & MIP_JOG )
                {
                    log_msg( prec, 0, "Stop jogging ..." );
                    prec->mip &= ~MIP_PAUSE;
                    prec->mip |=  MIP_STOP ;

                    send_msg( mInfo, "SL 0\r\nUs 0" );
                }
            }
            else if ( prec->mip == MIP_DONE )
            {
                VI = NINT( prec->vbas / prec->res );
                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );

                if ( prec->jogf > 0 )
                {
                    if ( prec->dir == imsDIR_Pos )
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL  %d", VI, A, VM );
                    else
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL -%d", VI, A, VM );

                    prec->mip  = MIP_JOGF;
                    log_msg( prec, 0, "Jogging forward ..." );
                }
                else
                {
                    if ( prec->dir == imsDIR_Pos )
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL -%d", VI, A, VM );
                    else
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL  %d", VI, A, VM );

                    prec->mip  = MIP_JOGR;
                    log_msg( prec, 0, "Jogging backward ..." );
                }
 
                prec->smov = 1;
                prec->dmov = 0;

                prec->lvio = 0;
                prec->rcnt = 0;
                prec->miss = 0;

                prec->athm = 0;
                msta.Bits.RA_HOMED = 0;
                msta.Bits.RA_HOME  = 0;
                msta.Bits.EA_HOME  = 0;

                db_post_events( prec, &prec->athm, DBE_VAL_LOG );

                send_msg( mInfo, msg );
            }

            break;
        case ( imsRecordDIR  ):
            if ( prec->dir == prec->oval ) break;

            if ( prec->dir == imsDIR_Pos )
            {
                log_msg( prec, 0, "Changed DIR from Negative to Positive" );

                prec->llm = prec->off + prec->dllm;
                prec->hlm = prec->off + prec->dhlm;
            }
            else
            {
                log_msg( prec, 0, "Changed DIR from Positive to Negative" );

                prec->llm = prec->off - prec->dhlm;
                prec->hlm = prec->off - prec->dllm;
            }

            nval       = prec->lls;
            prec->lls  = prec->hls;
            prec->hls  = (epicsInt16)nval;
            if ( prec->lls != nval )
            {
                db_post_events( prec, &prec->lls,  DBE_VAL_LOG );
                db_post_events( prec, &prec->hls,  DBE_VAL_LOG );
            }

            goto change_dir_off;
        case ( imsRecordOFF  ):
            log_msg( prec, 0, "Changed OFF from %.6g to %.6g", prec->oval,
                                                               prec->off  );

            prec->llm += prec->off - prec->oval;
            prec->hlm += prec->off - prec->oval;

            change_dir_off:
            db_post_events( prec, &prec->llm,  DBE_VAL_LOG );
            db_post_events( prec, &prec->hlm,  DBE_VAL_LOG );

            prec->rbv  = prec->drbv * (1. - 2.*prec->dir) + prec->off;
            prec->val  = prec->rbv;

//          check_software_limits( prec );

            break;
        case imsRecordSET :
            if ( (prec->set == prec->oval) || (prec->set == imsSET_Set) ) break;

            if ( (prec->val < prec->llm) || (prec->val > prec->hlm) )
                prec->lvio = 1;
            else
                prec->lvio = 0;

            break;
        case imsRecordHOMF:
            prec->homf = 0;

            if ( (prec->htyp == imsHTYP_None) ||
                 (prec->mip  != MIP_DONE    ) ||
                 (prec->spg  != imsSPG_Go   )    ) break;

            VI = NINT( prec->vbas / prec->res );
            if ( (prec->htyp == imsHTYP_Limits) ||
                 (prec->htyp == imsHTYP_Stall )    )
            {
                if ( prec->htyp == imsHTYP_Limits )
                {
                    if ( prec->mode == imsMode_Scan )
                    {
                        log_msg( prec, 0, "No homing to LS in scan mode" );
                        break;
                    }

                    log_msg( prec, 0, "Homing >> to HLS ..."   );
                }
                else
                    log_msg( prec, 0, "Homing >> to STALL ..." );

                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );

                prec->mip  = MIP_HOMF;
                if ( (prec->htyp == imsHTYP_Limits) && prec->hls )
                {
                    log_msg( prec, 0, "Already at HLS" );
                    log_msg( prec, 0, "Back off from HLS (HDST: %.6g), with ACCL & VELO",
                                      prec->hdst );

                    prec->mip  |= MIP_HOMB;
                    if ( prec->dir == imsDIR_Pos )
                        nval = -1. * prec->hdst / prec->res;
                    else
                        nval =       prec->hdst / prec->res;

                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nR2 0\r\nMR %ld",
                                  VI, VM, A, NINT(nval) );
                }
                else
                {
                    if ( prec->dir == imsDIR_Pos )
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL  %d", VI, A, VM );
                    else
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL -%d", VI, A, VM );
                }
            }
            else
            {
                if      ( prec->htyp == imsHTYP_Encoder )
                    log_msg( prec, 0, "Homing >> to encoder mark ..." );
                else if ( prec->htyp == imsHTYP_Switch  )
                    log_msg( prec, 0, "Homing >> to home switch ..."  );

                VM = NINT( prec->hvel / prec->res );
                A  = NINT( (prec->hvel - prec->vbas) / prec->res / prec->hacc );

                prec->mip  = MIP_HOMF;
                if      ( (prec->dir  == imsDIR_Pos) &&
                          (prec->hege == imsDIR_Pos)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 4",
                                  VI, VM, A, MI );
                else if ( (prec->dir  == imsDIR_Pos) &&
                          (prec->hege == imsDIR_Neg)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 3",
                                  VI, VM, A, MI );
                else if ( (prec->dir  == imsDIR_Neg) &&
                          (prec->hege == imsDIR_Pos)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 2",
                                  VI, VM, A, MI );
                else if ( (prec->dir  == imsDIR_Neg) &&
                          (prec->hege == imsDIR_Neg)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 1",
                                  VI, VM, A, MI );
            }
 
            prec->smov = 1;
            prec->dmov = 0;

            prec->lvio = 0;
            prec->rcnt = 0;
            prec->miss = 0;

            prec->athm = 0;
            msta.Bits.RA_HOMED = 0;
            msta.Bits.RA_HOME  = 0;
            msta.Bits.EA_HOME  = 0;

            db_post_events( prec, &prec->athm, DBE_VAL_LOG );

            send_msg( mInfo, msg    );
//          send_msg( mInfo, "Us 0" );

            break;
        case imsRecordHOMR:
            prec->homr = 0;

            if ( (prec->htyp == imsHTYP_None) ||
                 (prec->mip  != MIP_DONE    ) ||
                 (prec->spg  != imsSPG_Go   )    ) break;

            VI = NINT( prec->vbas / prec->res );
            if ( (prec->htyp == imsHTYP_Limits) ||
                 (prec->htyp == imsHTYP_Stall )    )
            {
                if ( prec->htyp == imsHTYP_Limits )
                {
                    if ( prec->mode == imsMode_Scan )
                    {
                        log_msg( prec, 0, "No homing to LS in scan mode" );
                        break;
                    }

                    log_msg( prec, 0, "Homing << to LLS ..."   );
                }
                else
                    log_msg( prec, 0, "Homing << to STALL ..." );

                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );

                prec->mip  = MIP_HOMR;
                if ( (prec->htyp == imsHTYP_Limits) && prec->lls )
                {
                    log_msg( prec, 0, "Already at LLS" );
                    log_msg( prec, 0, "Back off from LLS (HDST: %.6g), with ACCL & VELO",
                                      prec->hdst );

                    prec->mip  |= MIP_HOMB;
                    if ( prec->dir == imsDIR_Pos )
                        nval =       prec->hdst / prec->res;
                    else
                        nval = -1. * prec->hdst / prec->res;

                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nR2 0\r\nMR %ld",
                                  VI, VM, A, NINT(nval) );
                }
                else
                {
                    if ( prec->dir == imsDIR_Pos )
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL -%d", VI, A, VM );
                    else
                        sprintf( msg, "VI %d\r\nA %d\r\nD A\r\nSL  %d", VI, A, VM );
                }
            }
            else
            {
                if      ( prec->htyp == imsHTYP_Encoder )
                    log_msg( prec, 0, "Homing << to encoder mark ..." );
                else if ( prec->htyp == imsHTYP_Switch  )
                    log_msg( prec, 0, "Homing << to home switch ..."  );

                VM = NINT( prec->hvel / prec->res );
                A  = NINT( (prec->hvel - prec->vbas) / prec->res / prec->hacc );

                prec->mip  = MIP_HOMR;
                if      ( (prec->dir  == imsDIR_Pos) &&
                          (prec->hege == imsDIR_Pos)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 1",
                                  VI, VM, A, MI );
                else if ( (prec->dir  == imsDIR_Pos) &&
                          (prec->hege == imsDIR_Neg)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 2",
                                  VI, VM, A, MI );
                else if ( (prec->dir  == imsDIR_Neg) &&
                          (prec->hege == imsDIR_Pos)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 3",
                                  VI, VM, A, MI );
                else if ( (prec->dir  == imsDIR_Neg) &&
                          (prec->hege == imsDIR_Neg)    )
                    sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nH%c 4",
                                  VI, VM, A, MI );
            }
 
            prec->smov = 1;
            prec->dmov = 0;

            prec->lvio = 0;
            prec->rcnt = 0;
            prec->miss = 0;

            prec->athm = 0;
            msta.Bits.RA_HOMED = 0;
            msta.Bits.RA_HOME  = 0;
            msta.Bits.EA_HOME  = 0;

            db_post_events( prec, &prec->athm, DBE_VAL_LOG );

            send_msg( mInfo, msg    );
//          send_msg( mInfo, "Us 0" );

            break;
        case imsRecordHOMS:
            if ( (prec->athm == 0) || (prec->homs == 0) ) break;

            log_msg( prec, 0, "Set dial from %.6g to HOMD %.6g", prec->drbv,
                                                                 prec->homd );

            prec->homs = 0;
            prec->dval = prec->homd;
            prec->rval = NINT(prec->homd / prec->res);
            prec->val  = prec->homd * (1. - 2.*prec->dir) + prec->off;

            if ( prec->ee == imsAble_Enable )
            {
                log_msg( prec, 0, "Changed C2 from %ld to %ld", prec->rrbv,
                                                                prec->rval );

                new_c2 = prec->rval;
                new_c1 = NINT( prec->dval / prec->mres );
            }
            else
            {
                log_msg( prec, 0, "Changed C1 from %ld to %ld", prec->rrbv,
                                                                prec->rval );

                new_c1 = prec->rval;
                new_c2 = NINT( prec->dval / prec->eres );
            }
            sprintf( msg, "C1 %ld\r\nC2 %ld\r\nUs 0", new_c1, new_c2 );

            send_msg( mInfo, msg );

            break;
        case imsRecordCALF:
        case imsRecordCALR:
            if ( (prec->sevr >  MINOR_ALARM) || msta.Bits.RA_POWERUP ||
                 (prec->spg  != imsSPG_Go  )                            )
            {
                if      ( msta.Bits.RA_PROBLEM   )
                    log_msg( prec, 0, "No calib, hardware problem"  );
                else if ( msta.Bits.RA_NE        )
                    log_msg( prec, 0, "No calib, NE is set"         );
                else if ( msta.Bits.RA_BY0       )
                    log_msg( prec, 0, "No calib, MCode not running" );
                else if ( msta.Bits.NOT_INIT     )
                    log_msg( prec, 0, "No calib, init not finished" );
                else if ( msta.Bits.RA_POWERUP   )
                    log_msg( prec, 0, "No calib, power cycled"      );
                else if ( prec->spg != imsSPG_Go )
                    log_msg( prec, 0, "No calib, SPG is not Go"     );
                else
                    log_msg( prec, 0, "No calib, unknown alarm"     );

                break;
            }

            prec->lvio = 0;
            prec->rcnt = 0;
            prec->miss = 0;

            if ( fieldIndex == imsRecordCALF )
            {
                if ( prec->calf == 1 )
                {
                    log_msg( prec, 0, "Start calibration" );
                    prec->calf = 0;

                    prec->smov = 1;
                    prec->dmov = 0;
                    prec->mip  = MIP_CALI;

                    sprintf( msg, "C1 0\r\nC2 0\r\nMR  %d", prec->srev );
                    send_msg( mInfo, msg    );
//                  send_msg( mInfo, "Us 0" );
                }
                else
                {
                    if ( prec->dmov == 0 )                       // still moving
                    {
                        send_msg( mInfo, "SL 0" );
                        epicsThreadSleep( 0.5 );
                    }

                    sprintf( prec->cmd, "PR \"C2=\",C2" );
                    goto cmd_and_response;
                }
            }
            else
            {
                if ( prec->calr == 1 )
                {
                    log_msg( prec, 0, "Start calibration" );
                    prec->calr = 0;

                    prec->smov = 1;
                    prec->dmov = 0;
                    prec->mip  = MIP_CALI;

                    sprintf( msg, "C1 0\r\nC2 0\r\nMR -%d", prec->srev );
                    send_msg( mInfo, msg    );
//                  send_msg( mInfo, "Us 0" );
                }
                else
                {
                    if ( prec->dmov == 0 )                       // still moving
                    {
                        send_msg( mInfo, "SL 0" );
                        epicsThreadSleep( 0.5 );
                    }

                    sprintf( prec->cmd, "PR \"C2=\",C2" );
                    goto cmd_and_response;
                }
            }

            break;
        case imsRecordLLM :
            if ( prec->llm > prec->hlm )           // illegal, restore old value
            {
                prec->llm  = prec->oval;
                db_post_events( prec, &prec->llm,  DBE_VAL_LOG );
                break;
            }

            if ( prec->dir == imsDIR_Pos )
            {
                prec->dllm = prec->llm - prec->off;
                db_post_events( prec, &prec->dllm, DBE_VAL_LOG );
            }
            else
            {
                prec->dhlm = prec->off - prec->llm;
                db_post_events( prec, &prec->dhlm, DBE_VAL_LOG );
            }

            check_limit_violation:
            if ( (prec->val < prec->llm) || (prec->val > prec->hlm) )
                prec->lvio = 1;                   // set limit violation warning

            break;
        case imsRecordHLM :
            if ( prec->hlm < prec->llm )           // illegal, restore old value
            {
                prec->hlm  = prec->oval;
                db_post_events( prec, &prec->hlm,  DBE_VAL_LOG );
                break;
            }

            if ( prec->dir == imsDIR_Pos )
            {
                prec->dhlm = prec->hlm - prec->off;
                db_post_events( prec, &prec->dhlm, DBE_VAL_LOG );
            }
            else
            {
                prec->dllm = prec->off - prec->hlm;
                db_post_events( prec, &prec->dllm, DBE_VAL_LOG );
            }

            goto check_limit_violation;
        case imsRecordDLLM:
            if ( prec->dllm > prec->dhlm )         // illegal, restore old value
            {
                prec->dllm = prec->oval;
                db_post_events( prec, &prec->dllm, DBE_VAL_LOG );
                break;
            }

            if ( prec->dir == imsDIR_Pos )
            {
                prec->llm  = prec->off + prec->dllm;
                db_post_events( prec, &prec->llm,  DBE_VAL_LOG );
            }
            else
            {
                prec->hlm  = prec->off - prec->dllm;
                db_post_events( prec, &prec->hlm,  DBE_VAL_LOG );
            }

            goto check_limit_violation;
        case imsRecordDHLM:
            if ( prec->dhlm < prec->dllm )         // illegal, restore old value
            {
                prec->dhlm = prec->oval;
                db_post_events( prec, &prec->dhlm, DBE_VAL_LOG );
                break;
            }

            if ( prec->dir == imsDIR_Pos )
            {
                prec->hlm  = prec->off + prec->dhlm;
                db_post_events( prec, &prec->hlm,  DBE_VAL_LOG );
            }
            else
            {
                prec->llm  = prec->off - prec->dhlm;
                db_post_events( prec, &prec->llm,  DBE_VAL_LOG );
            }

            goto check_limit_violation;
        case imsRecordS1  :
            if      ( prec->s1 == imsS19_NotUsed) send_msg( mInfo, "S1 0,0,0" );
            else if ( prec->s1 == imsS19_HomeL  ) send_msg( mInfo, "S1 1,0,0" );
            else if ( prec->s1 == imsS19_HomeH  ) send_msg( mInfo, "S1 1,1,0" );
            else if ( prec->s1 == imsS19_LMTpL  ) send_msg( mInfo, "S1 2,0,0" );
            else if ( prec->s1 == imsS19_LMTpH  ) send_msg( mInfo, "S1 2,1,0" );
            else if ( prec->s1 == imsS19_LMTmL  ) send_msg( mInfo, "S1 3,0,0" );
            else if ( prec->s1 == imsS19_LMTmH  ) send_msg( mInfo, "S1 3,1,0" );
            else if ( prec->s1 == imsS19_5VOut  ) send_msg( mInfo, "S1 16,1,1");
            else if ( prec->s1 == imsS19_Brake  ) send_msg( mInfo, "S1 17,0,0");

            send_msg( mInfo, "Us 0");

            break;
        case imsRecordS2  :
            if      ( prec->s2 == imsS19_NotUsed) send_msg( mInfo, "S2 0,0,0" );
            else if ( prec->s2 == imsS19_HomeL  ) send_msg( mInfo, "S2 1,0,0" );
            else if ( prec->s2 == imsS19_HomeH  ) send_msg( mInfo, "S2 1,1,0" );
            else if ( prec->s2 == imsS19_LMTpL  ) send_msg( mInfo, "S2 2,0,0" );
            else if ( prec->s2 == imsS19_LMTpH  ) send_msg( mInfo, "S2 2,1,0" );
            else if ( prec->s2 == imsS19_LMTmL  ) send_msg( mInfo, "S2 3,0,0" );
            else if ( prec->s2 == imsS19_LMTmH  ) send_msg( mInfo, "S2 3,1,0" );
            else if ( prec->s2 == imsS19_5VOut  ) send_msg( mInfo, "S2 16,1,1");
            else if ( prec->s2 == imsS19_Brake  ) send_msg( mInfo, "S2 17,0,0");

            send_msg( mInfo, "Us 0");

            break;
        case imsRecordS3  :
            if      ( prec->s3 == imsS19_NotUsed) send_msg( mInfo, "S3 0,0,0" );
            else if ( prec->s3 == imsS19_HomeL  ) send_msg( mInfo, "S3 1,0,0" );
            else if ( prec->s3 == imsS19_HomeH  ) send_msg( mInfo, "S3 1,1,0" );
            else if ( prec->s3 == imsS19_LMTpL  ) send_msg( mInfo, "S3 2,0,0" );
            else if ( prec->s3 == imsS19_LMTpH  ) send_msg( mInfo, "S3 2,1,0" );
            else if ( prec->s3 == imsS19_LMTmL  ) send_msg( mInfo, "S3 3,0,0" );
            else if ( prec->s3 == imsS19_LMTmH  ) send_msg( mInfo, "S3 3,1,0" );
            else if ( prec->s3 == imsS19_5VOut  ) send_msg( mInfo, "S3 16,1,1");
            else if ( prec->s3 == imsS19_Brake  ) send_msg( mInfo, "S3 17,0,0");

            send_msg( mInfo, "Us 0");

            break;
        case imsRecordS4  :
            if      ( prec->s4 == imsS19_NotUsed) send_msg( mInfo, "S4 0,0,0" );
            else if ( prec->s4 == imsS19_HomeL  ) send_msg( mInfo, "S4 1,0,0" );
            else if ( prec->s4 == imsS19_HomeH  ) send_msg( mInfo, "S4 1,1,0" );
            else if ( prec->s4 == imsS19_LMTpL  ) send_msg( mInfo, "S4 2,0,0" );
            else if ( prec->s4 == imsS19_LMTpH  ) send_msg( mInfo, "S4 2,1,0" );
            else if ( prec->s4 == imsS19_LMTmL  ) send_msg( mInfo, "S4 3,0,0" );
            else if ( prec->s4 == imsS19_LMTmH  ) send_msg( mInfo, "S4 3,1,0" );
            else if ( prec->s4 == imsS19_5VOut  ) send_msg( mInfo, "S4 16,1,1");
            else if ( prec->s4 == imsS19_Brake  ) send_msg( mInfo, "S4 17,0,0");

            send_msg( mInfo, "Us 0");

            break;
        case imsRecordS9  :
            if      ( prec->s9 == imsS19_NotUsed) send_msg( mInfo, "S9 0,0,0" );
            else if ( prec->s9 == imsS19_HomeL  ) send_msg( mInfo, "S9 1,0,0" );
            else if ( prec->s9 == imsS19_HomeH  ) send_msg( mInfo, "S9 1,1,0" );
            else if ( prec->s9 == imsS19_LMTpL  ) send_msg( mInfo, "S9 2,0,0" );
            else if ( prec->s9 == imsS19_LMTpH  ) send_msg( mInfo, "S9 2,1,0" );
            else if ( prec->s9 == imsS19_LMTmL  ) send_msg( mInfo, "S9 3,0,0" );
            else if ( prec->s9 == imsS19_LMTmH  ) send_msg( mInfo, "S9 3,1,0" );
            else if ( prec->s9 == imsS19_5VOut  ) send_msg( mInfo, "S9 16,1,1");
            else if ( prec->s9 == imsS19_Brake  ) send_msg( mInfo, "S9 17,0,0");

            send_msg( mInfo, "Us 0");

            break;
        case imsRecordUREV:
            if ( prec->urev <= 0. )
            {
                prec->urev = prec->oval;
                db_post_events( prec, &prec->urev, DBE_VAL_LOG );
                break;
            }

            prec->mres = prec->urev / prec->srev;
            prec->eres = prec->urev / prec->el   / 4;
            db_post_events( prec, &prec->mres, DBE_VAL_LOG );
            db_post_events( prec, &prec->eres, DBE_VAL_LOG );

            set_urev:
            prec->vbas = prec->urev * prec->sbas;
            prec->vmax = prec->urev * prec->smax;
            prec->velo = prec->urev * prec->s   ;
            prec->bvel = prec->urev * prec->bs  ;
            prec->hvel = prec->urev * prec->hs  ;
            db_post_events( prec, &prec->vbas, DBE_VAL_LOG );
            db_post_events( prec, &prec->vmax, DBE_VAL_LOG );
            db_post_events( prec, &prec->velo, DBE_VAL_LOG );
            db_post_events( prec, &prec->bvel, DBE_VAL_LOG );
            db_post_events( prec, &prec->hvel, DBE_VAL_LOG );

            set_res:
            nval = prec->res;
            if ( prec->ee == imsAble_Enable ) prec->res = prec->eres;
            else                              prec->res = prec->mres;

            if ( prec->res != nval )
            {
                if ( prec->set == imsSET_Use )
                {
                    prec->drbv = prec->rrbv * prec->res;
                    prec->rbv  = (1. - 2.*prec->dir) * prec->drbv + prec->off;
                    prec->val  = prec->rbv;
                }

                db_post_events( prec, &prec->res,  DBE_VAL_LOG );
            }

            break;
        case imsRecordMS  :
            if ( prec->ms   <= 0 )
            {
                prec->ms   = (epicsInt16)prec->oval;
                db_post_events( prec, &prec->ms  , DBE_VAL_LOG );
                break;
            }

            sprintf( msg, "MS %d", prec->ms );
            send_msg( mInfo, msg );
        case imsRecordFREV:
            if ( prec->frev <= 0 )
            {
                prec->frev = (epicsInt16)prec->oval;
                db_post_events( prec, &prec->frev, DBE_VAL_LOG );
                break;
            }

            prec->srev = prec->frev * prec->ms  ;
            prec->mres = prec->urev / prec->srev;
            db_post_events( prec, &prec->srev, DBE_VAL_LOG );
            db_post_events( prec, &prec->mres, DBE_VAL_LOG );

            if ( prec->ee != imsAble_Enable ) goto set_res;

            break;
        case imsRecordSREV:
            if ( prec->srev <= 0 )
            {
                prec->srev = (epicsInt32)prec->oval;
                db_post_events( prec, &prec->srev, DBE_VAL_LOG );
                break;
            }

            prec->frev = prec->srev / prec->ms  ;
            prec->mres = prec->urev / prec->srev;
            db_post_events( prec, &prec->frev, DBE_VAL_LOG );
            db_post_events( prec, &prec->mres, DBE_VAL_LOG );

            if ( prec->ee != imsAble_Enable ) goto set_res;

            break;
        case imsRecordMRES:
            prec->urev = prec->srev * prec->mres;
            prec->eres = prec->urev / prec->el   / 4;
            db_post_events( prec, &prec->urev, DBE_VAL_LOG );
            db_post_events( prec, &prec->eres, DBE_VAL_LOG );

            goto set_urev;
        case imsRecordEL  :
            if ( prec->el   <= 0 )
            {
                prec->el   = prec->oval;
                db_post_events( prec, &prec->el  , DBE_VAL_LOG );
                break;
            }

            prec->eres = prec->urev / prec->el / 4;
            db_post_events( prec, &prec->eres, DBE_VAL_LOG );

            set_el:
            sprintf( msg, "EL %d", int(prec->el+0.5) );
            send_msg( mInfo, msg );

            if ( prec->ee == imsAble_Enable ) goto set_res;

            break;
        case imsRecordERES:
            prec->el   = prec->urev / prec->eres / 4;
            db_post_events( prec, &prec->el  , DBE_VAL_LOG );

            goto set_el;
        case imsRecordACCL:
            set_a_vi_vm:
            if ( prec->mode == imsMode_TrigMR )
            {
                VI = NINT( prec->vbas / prec->res );
                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );

                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A", VI, VM, A );

                send_msg( mInfo, msg );
            }

            break;
        case imsRecordSBAS:
            if ( (prec->smax > 0.) && (prec->sbas > prec->smax) )
            {
                prec->smax = prec->sbas;
                db_post_events( prec, &prec->smax, DBE_VAL_LOG );
            }
        case imsRecordSMAX:
            if ( (prec->smax > 0.) && (prec->smax < prec->sbas) )
            {
                prec->sbas = prec->smax;
                db_post_events( prec, &prec->sbas, DBE_VAL_LOG );
            }

            prec->vbas = prec->urev * prec->sbas;
            prec->vmax = prec->urev * prec->smax;
            db_post_events( prec, &prec->vbas, DBE_VAL_LOG );
            db_post_events( prec, &prec->vmax, DBE_VAL_LOG );

            goto enforce_Ss;
        case imsRecordVBAS:
            if ( (prec->vmax > 0.) && (prec->vbas > prec->vmax) )
            {
                prec->vmax = prec->vbas;
                db_post_events( prec, &prec->vmax, DBE_VAL_LOG );
            }
        case imsRecordVMAX:
            if ( (prec->vmax > 0.) && (prec->vmax < prec->vbas) )
            {
                prec->vbas = prec->vmax;
                db_post_events( prec, &prec->vbas, DBE_VAL_LOG );
            }

            prec->sbas = prec->vbas / prec->urev;
            prec->smax = prec->vmax / prec->urev;
            db_post_events( prec, &prec->sbas, DBE_VAL_LOG );
            db_post_events( prec, &prec->smax, DBE_VAL_LOG );

            enforce_Ss:
            enforce_S ( prec );
            enforce_BS( prec );
            enforce_HS( prec );

            goto set_a_vi_vm;
        case imsRecordS   :
            enforce_S ( prec );

            goto set_a_vi_vm;
        case imsRecordVELO:
            if      ( (prec->velo > prec->vmax) && (prec->vmax > 0.) )
            {
                prec->velo = prec->vmax;
                db_post_events( prec, &prec->velo, DBE_VAL_LOG );
            }
            else if ( prec->velo < prec->vbas )
            {
                prec->velo = prec->vbas;
                db_post_events( prec, &prec->velo, DBE_VAL_LOG );
            }

            prec->s    = prec->velo / prec->urev;
            db_post_events( prec, &prec->s   , DBE_VAL_LOG );

            goto set_a_vi_vm;
        case imsRecordBS  :
            enforce_BS( prec );

            break;
        case imsRecordBVEL:
            if      ( (prec->bvel > prec->vmax) && (prec->vmax > 0.) )
            {
                prec->bvel = prec->vmax;
                db_post_events( prec, &prec->bvel, DBE_VAL_LOG );
            }
            else if ( prec->bvel < prec->vbas )
            {
                prec->bvel = prec->vbas;
                db_post_events( prec, &prec->bvel, DBE_VAL_LOG );
            }

            prec->bs   = prec->bvel / prec->urev;
            db_post_events( prec, &prec->bs  , DBE_VAL_LOG );

            break;
        case imsRecordHS  :
            enforce_HS( prec );

            break;
        case imsRecordHVEL:
            if      ( (prec->hvel > prec->vmax) && (prec->vmax > 0.) )
            {
                prec->hvel = prec->vmax;
                db_post_events( prec, &prec->hvel, DBE_VAL_LOG );
            }
            else if ( prec->hvel < prec->vbas )
            {
                prec->hvel = prec->vbas;
                db_post_events( prec, &prec->hvel, DBE_VAL_LOG );
            }

            prec->hs   = prec->hvel / prec->urev;
            db_post_events( prec, &prec->hs  , DBE_VAL_LOG );

            break;
        case imsRecordEE  :
            if ( prec->ee   == prec->oval ) break;

            sprintf( msg, "EE %d", prec->ee );
            send_msg( mInfo, msg );

            nval = prec->res;
            if ( prec->ee   == imsAble_Enable ) prec->res = prec->eres;
            else                                prec->res = prec->mres;

            if ( prec->res  != nval )
                db_post_events( prec, &prec->res,  DBE_VAL_LOG );

            prec->rval = NINT( prec->dval / prec->res );
            VI         = NINT( prec->vbas / prec->res );
            VM         = NINT( prec->velo / prec->res );

            if ( prec->ee == imsAble_Enable )
            {
                new_c2 = prec->rval;
                new_c1 = NINT( prec->dval / prec->mres );
                sprintf( msg, "C1 %ld\r\nC2 %ld\r\nVI %d\r\nVM %d\r\nUs 0",
                              new_c1, new_c2, VI, VM );
            }
            else
            {
                new_c1 = prec->rval;
                new_c2 = NINT( prec->dval / prec->eres );
                sprintf( msg, "C1 %ld\r\nC2 %ld\r\nVM %d\r\nVI %d\r\nUs 0",
                              new_c1, new_c2, VM, VI );
            }

            send_msg( mInfo, msg );

            break;
        case imsRecordME  :
            sprintf( msg, "Me %d\r\nUs 0", prec->me );
            send_msg( mInfo, msg );

            break;
        case imsRecordLM  :
            sprintf( msg, "LM %d", prec->lm );
            send_msg( mInfo, msg );

            break;
        case imsRecordSM  :
            sprintf( msg, "SM %d", prec->sm );
            send_msg( mInfo, msg );

            break;
        case imsRecordSF  :
            sprintf( msg, "SF %d", prec->sf );
            send_msg( mInfo, msg );

            break;
        case imsRecordMT  :
            sprintf( msg, "MT %d", prec->mt );
            send_msg( mInfo, msg );

            break;
        case imsRecordHT  :
            sprintf( msg, "HT %d", prec->ht );
            send_msg( mInfo, msg );

            break;
        case imsRecordRCMX:
            if ( (prec->rcmx < 0) || (prec->rcmx > 100) )
            {
                prec->rcmx = (epicsInt16)prec->oval;
                db_post_events( prec, &prec->rcmx, DBE_VAL_LOG );

                break;
            }

            if ( prec->rc <= prec->rcmx ) break;

            prec->rc = prec->rcmx;
            db_post_events( prec, &prec->rc  , DBE_VAL_LOG );
        case imsRecordRC  :
            if ( (prec->rc   < 0) || (prec->rc   > prec->rcmx) )
            {
                prec->rc   = (epicsInt16)prec->oval;
                db_post_events( prec, &prec->rc  , DBE_VAL_LOG );

                break;
            }

            sprintf( msg, "RC %d", prec->rc );
            send_msg( mInfo, msg );

            break;
        case imsRecordHCMX:
            if ( (prec->hcmx < 0) || (prec->hcmx > 100) )
            {
                prec->hcmx = (epicsInt16)prec->oval;
                db_post_events( prec, &prec->hcmx, DBE_VAL_LOG );

                break;
            }

            if ( prec->hcsv > prec->hcmx )
            {
                prec->hcsv = prec->hcmx;
                db_post_events( prec, &prec->hcsv, DBE_VAL_LOG );
            }

            if ( prec->hc <= prec->hcmx ) break;

            prec->hc = prec->hcmx;
            db_post_events( prec, &prec->hc  , DBE_VAL_LOG );
        case imsRecordHC  :
            if ( (prec->hc   < 0) || (prec->hc   > prec->hcmx) )
            {
                prec->hc   = (epicsInt16)prec->oval;
                db_post_events( prec, &prec->hc  , DBE_VAL_LOG );

                break;
            }

            sprintf( msg, "HC %d", prec->hc );
            send_msg( mInfo, msg );

            if ( prec->hc > 0 )
            {
                prec->hcsv = prec->hc;
                prec->hctg = imsHC_Restore;
                db_post_events( prec, &prec->hcsv, DBE_VAL_LOG );
                db_post_events( prec, &prec->hctg, DBE_VAL_LOG );
            }
            else
            {
                prec->hctg = imsHC_Zero;
                db_post_events( prec, &prec->hctg, DBE_VAL_LOG );
            }

            break;
        case imsRecordHCTG:
            if ( prec->hctg == imsHC_Zero )
            {
                send_msg( mInfo, "HC 0" );

                prec->hc   = 0;
                db_post_events( prec, &prec->hc  , DBE_VAL_LOG );
            }
            else
            {
                sprintf( msg, "HC %d", prec->hcsv );
                send_msg( mInfo, msg    );

                prec->hc   = prec->hcsv;
                db_post_events( prec, &prec->hc  , DBE_VAL_LOG );
            }

            break;
        case imsRecordMODE:
            if ( prec->mode == prec->oval     ) break;

            if ( prec->mode == imsMode_TrigMR )
            {
                VI = NINT( prec->vbas / prec->res );
                VM = NINT( prec->velo / prec->res );
                A  = NINT( (prec->velo - prec->vbas) / prec->res / prec->accl );
                PT = NINT( (1. - 2.*prec->dir) * prec->twv / prec->res );

                sprintf( msg, "VI %d\r\nVM %d\r\nA %d\r\nD A\r\nPt %d\r\nTe 4",
                              VI, VM, A, PT );

                log_msg( prec, 0, "Waiting for trigger ..." );
            }
            else if ( prec->oval == imsMode_TrigMR )
            {
                sprintf( msg, "Te 0" );

                log_msg( prec, 0, "Disabled trigger"        );
            }

            send_msg( mInfo, msg );

            epicsThreadSleep( 0.1 );

            sprintf( msg, "Mm %d\r\nUs 0", prec->mode );
            send_msg( mInfo, msg );

            break;
        case imsRecordCMD :
            cmd_and_response:

            if ( strstr(prec->cmd, "PR ") || strstr(prec->cmd, "pr ") ||
                 strstr(prec->cmd, "Pr ") || strstr(prec->cmd, "pR ")    ) {
		status = send_recv_reply( mInfo, prec->cmd, rbbuf );
                if ( status > 0 ) strncpy( prec->resp, rbbuf, 61 );
                else              strncpy( prec->resp, "",    61 );
            } else {
		send_msg( mInfo, prec->cmd );
		strncpy( prec->resp, "", 61 );
            }

            db_post_events( prec,  prec->resp, DBE_VAL_LOG );

            break;
        case imsRecordEGAG:
            send_msg( mInfo, "Us 0" );

            break;
        case imsRecordRINI:
            log_msg( prec, 0, "Re-initialize ..." );
            post_msgs ( prec );

	    prec->msta = 0;
            init_motor( prec );

            prec->rini = 0;
            break;
    }

    if ( prec->val  != old_val  ) MARK( M_VAL  );
    if ( prec->dval != old_dval ) MARK( M_DVAL );
    if ( prec->rval != old_rval ) MARK( M_RVAL );
    if ( prec->rbv  != old_rbv  ) MARK( M_RBV  );
    if ( prec->mip  != old_mip  ) MARK( M_MIP  );
    if ( prec->dmov != old_dmov ) MARK( M_DMOV );
    if ( prec->rcnt != old_rcnt ) MARK( M_RCNT );
    if ( prec->miss != old_miss ) MARK( M_MISS );
    if ( prec->lvio != old_lvio ) MARK( M_LVIO );

    post_fields( prec, alarm_mask, 0 );
    post_msgs  ( prec                );

    return( 0 );
}

/******************************************************************************/
static long cvt_dbaddr( dbAddr *pDbAddr )
{
    imsRecord *prec = (imsRecord *)pDbAddr->precord;
    ims_info  *mInfo = (ims_info *)prec->dpvt;

    int        fieldIndex = dbGetFieldIndex( pDbAddr );
    long       status = 0;

    switch ( fieldIndex )
    {
        case imsRecordLOGA:
        {
            pDbAddr->pfield         = (char *)prec->loga;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGB:
        {
            pDbAddr->pfield         = (char *)prec->logb;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGC:
        {
            pDbAddr->pfield         = (char *)prec->logc;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGD:
        {
            pDbAddr->pfield         = (char *)prec->logd;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGE:
        {
            pDbAddr->pfield         = (char *)prec->loge;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGF:
        {
            pDbAddr->pfield         = (char *)prec->logf;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGG:
        {
            pDbAddr->pfield         = (char *)prec->logg;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
        case imsRecordLOGH:
        {
            pDbAddr->pfield         = (char *)prec->logh;
            pDbAddr->no_elements    = mInfo->mLength;
            pDbAddr->field_type     = DBF_CHAR;
            pDbAddr->field_size     = sizeof(char);
            pDbAddr->dbr_field_type = DBR_CHAR;
            break;
        }
    }

    return( status );
}

/******************************************************************************/
static long get_units( dbAddr *paddr, char *units )
{
    imsRecord *prec = (imsRecord *)paddr->precord;
    int  size = dbr_units_size - 1;       /* "dbr_units_size" from dbAccess.h */
    char s[30];

    int  fieldIndex = dbGetFieldIndex( paddr );

    switch ( fieldIndex )
    {
        case imsRecordSREV:
            strcpy ( s, "full steps/rev" );

            break;
        case imsRecordUREV:
            strncpy( s, prec->egu, DB_UNITS_SIZE );
            strcat ( s, "/rev" );

            break;
        case imsRecordACCL:
        case imsRecordBACC:
        case imsRecordHACC:
            strcpy ( s, "sec" );

            break;
        case imsRecordSBAS:
        case imsRecordSMAX:
        case imsRecordS:
        case imsRecordBS:
        case imsRecordHS:
            strcpy ( s, "rev/sec" );

            break;
        case imsRecordVBAS:
        case imsRecordVMAX:
        case imsRecordVELO:
        case imsRecordBVEL:
        case imsRecordHVEL:
            strncpy( s, prec->egu, DB_UNITS_SIZE );
            strcat ( s, "/sec" );

            break;
        default:
            strncpy( s, prec->egu, DB_UNITS_SIZE );
            break;
    }

    s[size] = '\0';
    strncpy( units, s, size+1 );

    return( 0 );
}

/******************************************************************************/
static long get_precision( const dbAddr *paddr, long *precision )
{
    imsRecord *prec = (imsRecord *)paddr->precord;
    int fieldIndex = dbGetFieldIndex( paddr );

    switch ( fieldIndex )
    {
        case imsRecordVERS:
            *precision = 2;

            break;
        case imsRecordRVAL:
        case imsRecordRRBV:
            *precision = 0;

            break;
        default:
            *precision = prec->prec;

            break;
    }

    return( 0 );
}

/******************************************************************************/
static long get_graphic_double( dbAddr *paddr, struct dbr_grDouble *pgd )
{
    imsRecord *prec = (imsRecord *)paddr->precord;

    int fieldIndex = dbGetFieldIndex( paddr );

    switch ( fieldIndex )
    {
        case imsRecordVAL:
        case imsRecordRBV:
            pgd->upper_disp_limit = prec->hlm;
            pgd->lower_disp_limit = prec->llm;

            break;
        case imsRecordDVAL:
        case imsRecordDRBV:
            pgd->upper_disp_limit = prec->dhlm;
            pgd->lower_disp_limit = prec->dllm;

            break;
        case imsRecordRVAL:
        case imsRecordRRBV:
            pgd->upper_disp_limit = prec->dhlm / prec->res;
            pgd->lower_disp_limit = prec->dllm / prec->res;

            break;
        case imsRecordS :
        case imsRecordBS:
        case imsRecordHS:
            pgd->upper_disp_limit = prec->smax;
            pgd->lower_disp_limit = prec->sbas;

            break;
        case imsRecordVELO:
        case imsRecordBVEL:
        case imsRecordHVEL:
            pgd->upper_disp_limit = prec->vmax;
            pgd->lower_disp_limit = prec->vbas;

            break;
        default:
            recGblGetGraphicDouble( (dbAddr *)paddr, pgd );

            break;
    }

    return( 0 );
}

/******************************************************************************/
static long get_control_double( dbAddr *paddr, struct dbr_ctrlDouble *pcd )
{
    imsRecord *prec = (imsRecord *)paddr->precord;

    int fieldIndex = dbGetFieldIndex( paddr );

    switch ( fieldIndex )
    {
        case imsRecordVAL:
        case imsRecordRBV:
            pcd->upper_ctrl_limit = prec->hlm;
            pcd->lower_ctrl_limit = prec->llm;

            break;
        case imsRecordDVAL:
        case imsRecordDRBV:
            pcd->upper_ctrl_limit = prec->dhlm;
            pcd->lower_ctrl_limit = prec->dllm;

            break;
        case imsRecordRVAL:
        case imsRecordRRBV:
            pcd->upper_ctrl_limit = prec->dhlm / prec->res;
            pcd->lower_ctrl_limit = prec->dllm / prec->res;

            break;
        case imsRecordS :
        case imsRecordBS:
        case imsRecordHS:
            pcd->upper_ctrl_limit = prec->smax;
            pcd->lower_ctrl_limit = prec->sbas;

            break;
        case imsRecordVELO:
        case imsRecordBVEL:
        case imsRecordHVEL:
            pcd->upper_ctrl_limit = prec->vmax;
            pcd->lower_ctrl_limit = prec->vbas;

            break;
        default:
            recGblGetControlDouble( (dbAddr *)paddr, pcd );
            break;
    }

    return( 0 );
}

/******************************************************************************/
static long get_alarm_double( dbAddr *paddr, struct dbr_alDouble *pad )
{
    return( 0 );
}

/******************************************************************************/
/* Assume rMutex is locked! */
static struct ims_asyn_req *get_request_buf( struct ims_info *mInfo )
{
    int r = mInfo->reqH;
    struct ims_asyn_req *req;

    if (r == -1) /* This should never happen.  Famous last... */
        return NULL;
    req = &mInfo->req[r];
    mInfo->reqH = req->link;
    return req;
}

/* Assume rMutex is locked! */
static void free_request_buf ( struct ims_info *mInfo, struct ims_asyn_req *req )
{
    int r = req - &mInfo->req[0];
    req->state = IAR_EMPTY;
    req->link = mInfo->reqH;
    mInfo->reqH = r;
}

static long send_msg( struct ims_info *mInfo, char const *msg )
{
    struct ims_asyn_req *req;

    mInfo->rMutex->lock();
    req = get_request_buf(mInfo);
    if (!req) {
	mInfo->rMutex->unlock();
	return -1;
    }

    sprintf(req->out, "%s\r", msg);
    req->outsize = strlen(req->out);
    req->state = IAR_SENDONLY;
    mInfo->rMutex->unlock();
    return( 0 );
}

/******************************************************************************/
/*
 * The new regime: all of our PR statements in *code* start 'PR "XY=",' for some
 * unique characters XY.  However... since we allow users to just send arbitrary
 * commands, we can't really count on *all* of the commands looking like this.
 * But if it *doesn't* look like this, we know it has to be a user command!
 */
static long send_recv_reply( struct ims_info *mInfo, char const *msg, char *rbbuf)
{
    struct ims_asyn_req *req;
    int status;

    mInfo->rMutex->lock();
    req = get_request_buf(mInfo);
    if (!req) {
	mInfo->rMutex->unlock();
	*rbbuf = 0;
	return 0;
    }

    sprintf(req->out, "%s\r", msg);
    req->outsize = strlen(req->out);
    req->state = IAR_SEND;
    /*
     * We wouldn't be here if msg didn't start "PR ". Only question is
     * if we are printing a literal string after this.
     *
     * We're trying to be safe here in case the user types something
     * like 'PR ""' or 'PR "X"'.
     */
    if (msg[3] == '"') {
	req->exp[0] = msg[4] == '"' ? 0 : msg[4];
	req->exp[1] = msg[5] == '"' ? 0 : msg[5];
	req->exp[2] = 0;
    } else {
	req->exp[0] = 0;
    }
    req->rbbuf = rbbuf;
    req->state = IAR_SEND;
    mInfo->rMutex->unlock();

    status = req->rbEvent->wait(1.0);
    mInfo->rMutex->lock();
    /*
     * This is pure paranoia.  There might be a race here, if we return from
     * the wait with a failure, but then the event is signaled before we can
     * grab the lock.
     */
    if (!status) {
	status = req->rbEvent->tryWait();
    }
    if (status) {
	status = req->nread;
    }
    free_request_buf(mInfo, req);
    mInfo->rMutex->unlock();

    return( status );
}

/******************************************************************************/
static void post_fields( imsRecord *prec, unsigned short alarm_mask,
                                          unsigned short all )
{
    unsigned short  field_mask;
    changed_fields  cmap;

    cmap.All = prec->cmap;

    if ( (field_mask = alarm_mask | (all                  ? DBE_VAL_LOG : 0)) )
    {
        db_post_events( prec, &prec->vers, field_mask );
        db_post_events( prec,  prec->desc, field_mask );
        db_post_events( prec,  prec->port, field_mask );
        db_post_events( prec,  prec->asyn, field_mask );
        db_post_events( prec,  prec->pn,   field_mask );
        db_post_events( prec, &prec->ee,   field_mask );
        db_post_events( prec, &prec->sm,   field_mask );
        db_post_events( prec,  prec->egu,  field_mask );
        db_post_events( prec, &prec->dllm, field_mask );
        db_post_events( prec, &prec->dhlm, field_mask );
        db_post_events( prec, &prec->llm,  field_mask );
        db_post_events( prec, &prec->hlm,  field_mask );
/*
        db_post_events( prec, &prec->svel, field_mask );
        db_post_events( prec, &prec->sacc, field_mask );
        db_post_events( prec, &prec->velo, field_mask );
        db_post_events( prec, &prec->accl, field_mask );
        db_post_events( prec, &prec->shve, field_mask );
        db_post_events( prec, &prec->shac, field_mask );
        db_post_events( prec, &prec->hvel, field_mask );
        db_post_events( prec, &prec->hacc, field_mask );
        db_post_events( prec, &prec->bdst, field_mask );
        db_post_events( prec, &prec->dir,  field_mask );
        db_post_events( prec, &prec->off,  field_mask );
        db_post_events( prec, &prec->set,  field_mask );
        db_post_events( prec, &prec->twv,  field_mask );
        db_post_events( prec, &prec->twf,  field_mask );
        db_post_events( prec, &prec->twr,  field_mask );
        db_post_events( prec, &prec->rtry, field_mask );
        db_post_events( prec, &prec->rdbd, field_mask );
        db_post_events( prec, &prec->pdbd, field_mask );
        db_post_events( prec, &prec->spg,  field_mask );
        db_post_events( prec, &prec->home, field_mask );
        db_post_events( prec, &prec->refr, field_mask );
        db_post_events( prec, &prec->init, field_mask );
*/
    }

    if ( (field_mask = alarm_mask | (all | MARKED(M_VAL ) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->val,  field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_DVAL) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->dval, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_RVAL) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->rval, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_RRBV) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->rrbv, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_DRBV) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->drbv, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_RBV ) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->rbv,  field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_DIFF) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->diff, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_MIP ) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->mip,  field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_MOVN) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->movn, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_DMOV) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->dmov, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_RCNT) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->rcnt, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_MISS) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->miss, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_LVIO) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->lvio, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_MSTA) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->msta, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_MSTR) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec,  prec->mstr, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_RLLS) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->rlls, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_RHLS) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->rhls, field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_LLS ) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->lls,  field_mask );

    if ( (field_mask = alarm_mask | (all | MARKED(M_HLS ) ? DBE_VAL_LOG : 0)) )
        db_post_events( prec, &prec->hls,  field_mask );

    UNMARK_ALL;

    return;
}

/******************************************************************************/
/* Debug routine */
#ifdef DEBUG
static void print_string(char *prefix, char *buf, size_t len)
{
    printf(prefix);
    for (size_t ii = 0; ii < len; ii++) {
	if (buf[ii] == '\r')
	    printf("\\r");
	else if (buf[ii] == '\n')
	    printf("\\n");
	else
	    putchar(buf[ii]);
    }
    putchar('\n');
}
#endif

/******************************************************************************/
/* Queue a dbPutField request.  This is understood to be a single string. */
static void queue_pf_req(struct ims_info *mInfo, struct dbAddr *addr, char *msg)
{
    struct pf_asyn_req req;
    req.addr = addr;
    strncpy(req.out, msg, sizeof(req.out));
    mInfo->pfq->send((void *)&req, sizeof(struct pf_asyn_req));
}

/******************************************************************************/
static void pf_controller ( struct ims_info *mInfo )
{
    struct pf_asyn_req req;

    for (;;) {
	if (mInfo->pfq->receive(&req, sizeof(req)) != sizeof(req)) {
	    /* This should never happen, but if it does, let's not just spin. */
	    epicsThreadSleep( 0.2 );
	    continue;
	}
	dbPutField(req.addr, DBR_STRING, req.out, 1);
    }
}

/******************************************************************************/
static void io_controller ( struct ims_info *mInfo )
{
    char buf[2048];
    const double wtimeout = 1.0;
    const double rtimeout = 0.1;
    asynStatus status;
    int i, eomReason, wild;
    size_t len;

    for (;;) {
	/* Check for new work */
	mInfo->rMutex->lock();
	for (i = 0; i < REQ_CNT; i++) {
	    struct ims_asyn_req *req = &mInfo->req[i];

	    if (req->state == IAR_SENDONLY) {
		pasynOctetSyncIO->write( mInfo->pasynUser, req->out, 
					 req->outsize, wtimeout, &len );
		free_request_buf(mInfo, req);
		continue;
	    }
	    if (req->state == IAR_SEND) {
		pasynOctetSyncIO->write( mInfo->pasynUser, req->out, 
					 req->outsize, wtimeout, &len );
		req->state = IAR_RESPWAIT;
		continue;
	    }
	}
	mInfo->rMutex->unlock();

	/* Try to read a message */
	status = pasynOctetSyncIO->read( mInfo->pasynUser, buf, sizeof(buf) - 1,
					 rtimeout, &len, &eomReason );
	if (status != asynSuccess || len <= 0)
	    continue;

	if (len < sizeof(buf))
	    buf[len] = 0;

#ifdef DEBUG
	print_string((char *)"< ", buf, len);
#endif
	if (!strncmp(buf, "BOS", 3)) {
	    /*
	     * BOSstatus,P=positionEOS
	     * Strip off "BOS" and pass it to $(MOTOR).SSTR.
	     */
	    if (!mInfo->sstr_status) {
		queue_pf_req(mInfo, &mInfo->sstr_addr, buf+3);
	    }
	    continue;
	}
	if (!strncmp(buf, "Want2Save", 9)) {
	    /*
	     * Want2Save
	     * Strip off "Want2" and pass it to $(MOTOR).SVNG.
	     */
	    if (!mInfo->svng_status) {
		queue_pf_req(mInfo, &mInfo->svng_addr, buf+5);
	    }
	    continue;
	}
	if (!strncmp(buf, "Saved ", 6)) {
	    /*
	     * Saved position
	     * Strip off "Saved " and pass it to $(MOTOR).SVNG and
	     * $(MOTOR).SAVED_STR.
	     */
	    if (!mInfo->svng_status) {
		queue_pf_req(mInfo, &mInfo->svng_addr, buf+6);
	    }
	    if (!mInfo->saved_status) {
		queue_pf_req(mInfo, &mInfo->saved_addr, buf+6);
	    }
	    continue;
	}
	mInfo->rMutex->lock();
	/*
	 * Check if this is a response to something previously sent.
	 */
	for (wild = -1, i = 0; i < REQ_CNT; i++) {
	    struct ims_asyn_req *req = &mInfo->req[i];
	    int explen = strlen(req->exp);
	    if (req->state == IAR_RESPWAIT && !explen) {
		wild = i; /* Just remember that this is our potential wildcard match. */
		continue;
	    }
	    /*
	     * Sigh.  This is fucked up.
	     *
	     * The IMS motor firmware seems to have a bug in it... when you send
	     *     PR "PN=",PN
	     * The response can be:
	     *     MDI3CRL23C7-EQPN=
	     * Let's assume that our expected length is always 2, and... sigh.
	     */
	    if (req->state == IAR_RESPWAIT && 
		(!strncmp(buf, req->exp, explen) ||
		 (buf[len-1] == '=' && 
		  !strncmp(buf+len-1-strlen(req->exp), req->exp, explen)))) {
		req->nread = strlen(buf);
		if (req->nread >= MAX_MSG_SIZE)
		    req->nread = MAX_MSG_SIZE-1;
		strncpy(req->rbbuf, buf, req->nread);
		req->rbbuf[req->nread] = 0;
		req->state = IAR_DONE;
		req->rbEvent->signal();
		break;
	    }
	}
	/*
	 * If we didn't match anything but there is a wild card match,
	 * match it!
	 */
	if (i == REQ_CNT && wild >= 0) {
	    struct ims_asyn_req *req = &mInfo->req[wild];
	    req->nread = strlen(buf);
	    if (req->nread >= MAX_MSG_SIZE)
		req->nread = MAX_MSG_SIZE-1;
	    strncpy(req->rbbuf, buf, req->nread);
	    req->rbbuf[req->nread] = 0;
	    req->state = IAR_DONE;
	    req->rbEvent->signal();
	}
	mInfo->rMutex->unlock();
    }
}

/******************************************************************************/
static void ping_controller( struct ims_info *mInfo )
{
    imsRecord    *prec = mInfo->precord;
    char          msg[MAX_MSG_SIZE];
    long          status = OK;
    int           retry, rbby;
    motor_status  msta;

    while ( 1 )
    {
        mInfo->pEvent->wait();

        msta.All = 0;

        // check the MCode running status
        retry = 1;
        do
        {
	    status = send_recv_reply( mInfo, "PR \"PI=\",BY", msg );
            if ( status > 0 )
            {
                status = sscanf( msg, "PI=%d", &rbby );
                if ( status == 1 ) break;
            }

            epicsThreadSleep( 0.2 );
        } while ( retry++ < 3 );

        if ( status == 1 )                                       // read back BY
        {
            if      ( rbby == 0 )                           // MCode not running
            {
                msta.Bits.RA_BY0     = 1;
            }
            else if ( rbby != 1 )                              // wrong BY value
            {
                log_msg( prec, 0, "Invalid BY readback" );
                msta.Bits.RA_PROBLEM = 1;
            }
        }
        else
        {
            log_msg( prec, 0, "Can not reach controller" );
            msta.Bits.RA_PROBLEM = 1;
        }

        mInfo->cMutex->lock();

        mInfo->csr     = msta.All;
        mInfo->newData = 2;

        mInfo->cMutex->unlock();

        dbScanLock  ( (dbCommon *)prec );
        process     ( (dbCommon *)prec );
        dbScanUnlock( (dbCommon *)prec );
    }
}

/******************************************************************************/
static long log_msg( imsRecord *prec, int dlvl, const char *fmt, ... )
{
    ims_info  *mInfo = (ims_info *)prec->dpvt;
    timespec   ts;
    struct tm  timeinfo;
    char       timestamp[40], msec[4], msg[512];

    va_list    args;

    if ( (dlvl >= 0) && (prec->mode != imsMode_Scan        ) &&
         (dlvl >  max((int)prec->dlvl, (int)imsRecordDebug))    ) return( 0 );

    if ( (dlvl >= 0) && (prec->mode == imsMode_Scan        ) &&
         (dlvl >  min((int)prec->dlvl, 0                  ))    ) return( 0 );

    clock_gettime( CLOCK_REALTIME, &ts );
    localtime_r( &ts.tv_sec, &timeinfo );

    strftime( timestamp, 40, "%m/%d %H:%M:%S", &timeinfo );
    sprintf ( msec, "%03d", int(ts.tv_nsec*1.e-6 + 0.5) );

    va_start( args, fmt      );
    vsprintf( msg, fmt, args );
    va_end  ( args           );

    if ( (dlvl >= 0) && (((prec->mode != imsMode_Scan           ) &&
                          (dlvl       <= prec->dlvl             )    ) ||
                         ((prec->mode == imsMode_Scan           ) &&
                          (dlvl       <= min((int)prec->dlvl, 0))    )    ) )
    {
        mInfo->lMutex->lock();

        if ( mInfo->cIndex > 0 )
        {
            if ( strncmp(mInfo->sAddr+mInfo->mLength*(mInfo->cIndex-1)+9,
                         msg, strlen(msg)                                )==0 )
                mInfo->cIndex--;
            else if ( mInfo->cIndex > 7 )
                memmove( mInfo->sAddr,
                         mInfo->sAddr+mInfo->mLength, mInfo->mLength*7 );
        }

        snprintf( mInfo->sAddr+mInfo->mLength*min(mInfo->cIndex,7), 61,
                  "%s %s", timestamp+6, msg );

        if ( mInfo->cIndex <= 7 ) mInfo->cIndex++;

        mInfo->newMsg = 1;

        mInfo->lMutex->unlock();
    }

    if ( (dlvl < 0)                                                 ||
         ((prec->mode != imsMode_Scan) && (dlvl <= imsRecordDebug))    )
        printf( "%s.%s %s -- %s\n", timestamp, msec, prec->name, msg );

    return( 1 );
}

/******************************************************************************/
static void post_msgs( imsRecord *prec )
{
    ims_info  *mInfo = (ims_info *)prec->dpvt;

    mInfo->lMutex->lock();

    if ( mInfo->newMsg )
    {
        db_post_events( prec,  prec->loga, DBE_VAL_LOG );
        db_post_events( prec,  prec->logb, DBE_VAL_LOG );
        db_post_events( prec,  prec->logc, DBE_VAL_LOG );
        db_post_events( prec,  prec->logd, DBE_VAL_LOG );
        db_post_events( prec,  prec->loge, DBE_VAL_LOG );
        db_post_events( prec,  prec->logf, DBE_VAL_LOG );
        db_post_events( prec,  prec->logg, DBE_VAL_LOG );
        db_post_events( prec,  prec->logh, DBE_VAL_LOG );

        mInfo->newMsg = 0;
    }

    mInfo->lMutex->unlock();

    return;
}

/******************************************************************************/
static void enforce_S ( imsRecord *prec )
{
    if      ( (prec->s    > prec->smax) && (prec->smax > 0.) )
    {
        prec->s    = prec->smax;
        db_post_events( prec, &prec->s,    DBE_VAL_LOG );
    }
    else if ( prec->s    < prec->sbas )
    {
        prec->s    = prec->sbas;
        db_post_events( prec, &prec->s,    DBE_VAL_LOG );
    }

    prec->velo = prec->urev * prec->s   ;
    db_post_events( prec, &prec->velo, DBE_VAL_LOG );

    return;
}

/******************************************************************************/
static void enforce_BS( imsRecord *prec )
{
    if      ( (prec->bs   > prec->smax) && (prec->smax > 0.) )
    {
        prec->bs   = prec->smax;
        db_post_events( prec, &prec->bs  , DBE_VAL_LOG );
    }
    else if ( prec->bs   < prec->sbas )
    {
        prec->bs   = prec->sbas;
        db_post_events( prec, &prec->bs  , DBE_VAL_LOG );
    }

    prec->bvel = prec->urev * prec->bs  ;
    db_post_events( prec, &prec->bvel, DBE_VAL_LOG );

    return;
}

/******************************************************************************/
static void enforce_HS( imsRecord *prec )
{
    if      ( (prec->hs   > prec->smax) && (prec->smax > 0.) )
    {
        prec->hs   = prec->smax;
        db_post_events( prec, &prec->hs  , DBE_VAL_LOG );
    }
    else if ( prec->hs   < prec->sbas )
    {
        prec->hs   = prec->sbas;
        db_post_events( prec, &prec->hs  , DBE_VAL_LOG );
    }

    prec->hvel = prec->urev * prec->hs  ;
    db_post_events( prec, &prec->hvel, DBE_VAL_LOG );

    return;
}
