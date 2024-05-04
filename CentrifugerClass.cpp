#include <stdio.h>
#include <time.h>
#include <getopt.h>

#include "argvdefs.h"
#include "ReadFiles.hpp"
#include "compactds/Sequence_Hybrid.hpp"
#include "compactds/FMBuilder.hpp"
#include "compactds/FMIndex.hpp"
#include "compactds/Alphabet.hpp"

#include "Taxonomy.hpp"
#include "Classifier.hpp"
#include "ResultWriter.hpp"
#include "ReadPairMerger.hpp"
#include "ReadFormatter.hpp"
#include "BarcodeCorrector.hpp"
#include "BarcodeTranslator.hpp"

char usage[] = "./centrifuger [OPTIONS]:\n"
  "Required:\n"
  "\t-x FILE: index prefix\n"
  "\t-1 FILE -2 FILE: paired-end read\n"
  "\t-u FILE: single-end read\n"
  //"\t--sample-sheet FILE: \n"
  "Optional:\n"
  //"\t-o STRING: output prefix [centrifuger]\n"
  "\t-t INT: number of threads [1]\n"
  "\t-k INT: report upto <int> distinct, primary assignments for each read pair [1]\n"
  "\t--barcode STR: path to the barcode file\n"
  "\t--UMI STR: path to the UMI file\n"
  "\t--read-format STR: format for read, barcode and UMI files, e.g. r1:0:-1,r2:0:-1,bc:0:15,um:16:-1 for paired-end files with barcode and UMI\n"
  "\t--min-hitlen INT: minimum length of partial hits [auto]\n"
  "\t--hitk-factor INT: resolve at most <int>*k entries for each hit [40; use 0 for no restriction]\n"
  "\t--merge-readpair: merge overlapped paired-end reads and trim adapters [no merge]\n"
  "\t--barcode-whitelist STR: path to the barcode whitelist file.\n"
  "\t--barcode-translate STR: path to the barcode translation file.\n"
  "\t-v: print the version information and quit\n"
  ;

static const char *short_options = "x:1:2:u:o:t:k:v" ;
static struct option long_options[] = {
  { "min-hitlen", required_argument, 0, ARGV_MIN_HITLEN},
  { "hitk-factor", required_argument, 0, ARGV_MAX_RESULT_PER_HIT_FACTOR},
  { "merge-readpair", no_argument, 0, ARGV_MERGE_READ_PAIR },
  { "read-format", required_argument, 0, ARGV_READFORMAT},
  { "barcode", required_argument, 0, ARGV_BARCODE},
  { "UMI", required_argument, 0, ARGV_UMI},
  { "barcode-whitelist", required_argument, 0, ARGV_BARCODE_WHITELIST},
  { "barcode-translate", required_argument, 0, ARGV_BARCODE_TRANSLATE},
  { (char *)0, 0, 0, 0} 
} ;

struct _inputThreadArg
{
  ReadFiles *reads, *mateReads, *barcodeFile, *umiFile ;
  struct _Read *readBatch, *readBatch2, *barcodeBatch, *umiBatch ;

  ReadFormatter *readFormatter ;
  BarcodeCorrector *barcodeCorrector ;
  BarcodeTranslator *barcodeTranslator ;

  int maxBatchSize ;
  int *pBatchSize ;
} ;

struct _threadArg 
{
  struct _Read *readBatch, *readBatch2 ;

  int threadCnt ;
  int batchSize ;

  ReadPairMerger *readPairMerger ;

  Classifier *classifier ;
  struct _classifierResult *results ;

  int tid ;
} ;

int GetReadBatch(ReadFiles &reads, struct _Read *readBatch, 
    ReadFiles &mateReads, struct _Read *readBatch2, 
    ReadFiles &barcodeFile, struct _Read *barcodeBatch, 
    ReadFiles &umiFile, struct _Read *umiBatch, 
    ReadFormatter &readFormatter, BarcodeCorrector &barcodeCorrector, 
    BarcodeTranslator &barcodeTranslator, int maxBatchSize)
{
  int i ;
  int fileInd1, fileInd2, fileIndBc, fileIndUmi ;

  int batchSize = reads.GetBatch( readBatch, maxBatchSize, fileInd1, true, true ) ;
  if ( readBatch2 != NULL )
  {
    int tmp = mateReads.GetBatch( readBatch2, maxBatchSize, fileInd2, true, true ) ;
    if ( tmp != batchSize )
    {
      Utils::PrintLog("ERROR: The two mate-pair read files have different number of reads." ) ;
      exit(EXIT_FAILURE) ;
    }
  }

  if (barcodeBatch != NULL)
  {
    int tmp = barcodeFile.GetBatch( barcodeBatch, maxBatchSize, fileIndBc, true, true ) ;
    if ( tmp != batchSize )
    {
      Utils::PrintLog("ERROR: The barcode file and read file have different number of reads." ) ;
      exit(EXIT_FAILURE) ;
    }
  }
  
  if (umiBatch != NULL)
  {
    int tmp = umiFile.GetBatch( umiBatch, maxBatchSize, fileIndUmi, true, true ) ;
    if ( tmp != batchSize )
    {
      Utils::PrintLog("ERROR: The UMI file and read file have different number of reads." ) ;
      exit(EXIT_FAILURE) ;
    }
  }

  // Reformat everything
  for (i = 0 ; i < batchSize ; ++i)
  {
    // No need to worry buffer id for now, as the parsing input part is sequential.
    readFormatter.InplaceExtractSeqAndQual(readBatch[i].seq, readBatch[i].qual, FORMAT_READ1) ;
    if (readBatch2 != NULL)
      readFormatter.InplaceExtractSeqAndQual(readBatch2[i].seq, readBatch2[i].qual, FORMAT_READ2) ;
    if (barcodeBatch != NULL)
    {
      readFormatter.InplaceExtractSeqAndQual(barcodeBatch[i].seq, barcodeBatch[i].qual, FORMAT_BARCODE) ;
      
      char *barcode = barcodeBatch[i].seq ;
      char *qual = barcodeBatch[i].qual ;

      int result = 0 ;
      if (barcodeCorrector.GetWhitelistSize() > 0)
        result = barcodeCorrector.Correct(barcode, qual) ;
      if (result >= 0)
      {
        if (barcodeTranslator.IsSet())
        {
          std::string newbc = barcodeTranslator.Translate(barcode, strlen(barcode)) ;
          free(barcodeBatch[i].seq) ;
          barcodeBatch[i].seq = strdup(newbc.c_str()) ;
        }
      }
      else // not in whitelist
      {
        barcode[0] = 'N' ;
        barcode[1] = '\0' ;
      }
    }
    if (umiBatch != NULL)
      readFormatter.InplaceExtractSeqAndQual(umiBatch[i].seq, umiBatch[i].qual, FORMAT_UMI) ;
  }
  return batchSize ;
}

void *LoadReads_Thread(void *pArg)
{
  struct _inputThreadArg &arg = *((struct _inputThreadArg *)pArg);
  *(arg.pBatchSize) = GetReadBatch(*(arg.reads), arg.readBatch, 
      *(arg.mateReads), arg.readBatch2,
      *(arg.barcodeFile), arg.barcodeBatch,
      *(arg.umiFile), arg.umiBatch,
      *(arg.readFormatter), *(arg.barcodeCorrector), *(arg.barcodeTranslator),
      arg.maxBatchSize) ;

  pthread_exit(NULL) ;
}

void *ClassifyReads_Thread(void *pArg)
{
  int i ;
  struct _threadArg &arg = *((struct _threadArg *)pArg);
  for (i = 0 ; i < arg.batchSize ; ++i)
  {
    if (i % arg.threadCnt != arg.tid)
      continue ;
    
    // Merge two read pairs
    char *r1, *q1, *r2, *q2 ;
    char *rm, *qm ;
    
    r1 = arg.readBatch[i].seq ;
    q1 = arg.readBatch[i].qual ;

    r2 = NULL ;
    q2 = NULL ;
    if (arg.readBatch2)
    {
      r2 = arg.readBatch2[i].seq ;
      q2 = arg.readBatch2[i].qual ;
    }

    int mergeResult = 0 ;
    if (arg.readPairMerger != NULL)
      mergeResult = arg.readPairMerger->Merge(r1, q1, r2, q2, &rm, &qm) ;

    if (mergeResult == 0)
    {
      arg.classifier->Query(r1, r2, arg.results[i]) ;
    }
    else
    {
      arg.classifier->Query(rm, NULL, arg.results[i]) ;
      free(rm) ;
      if (qm)
        free(qm) ;
    }

    //arg.classifier->Query(arg.readBatch[i].seq, arg.readBatch2 ? arg.readBatch2[i].seq : NULL, arg.results[i]) ;
  }
  pthread_exit(NULL) ;
}

int main(int argc, char *argv[])
{
  int i ;

  if ( argc <= 1 )
  {
    fprintf( stderr, "%s", usage ) ;
    return 0 ;
  }

  int c, option_index ;
  option_index = 0 ;
  
  char outputPrefix[1024] = "centrifuger" ;
  char *idxPrefix = NULL ;
  int threadCnt = 1 ;
  Classifier classifier ;
  struct _classifierParam classifierParam ;
  ReadFiles reads ;
  ReadFiles mateReads ;
  bool hasMate = false ;
  ResultWriter resWriter ;
  ReadPairMerger readPairMerger ;
  bool mergeReadPair = false ;
  
  // variables regarding barcode, UMI
  ReadFiles barcodeFile ;
  ReadFiles umiFile ;
  ReadFormatter readFormatter ;
  BarcodeCorrector barcodeCorrector ;
  BarcodeTranslator barcodeTranslator ;
  bool hasBarcode = false ;
  bool hasBarcodeWhitelist = false ;
  bool hasUmi = false ;

  while (1)
  {
    c = getopt_long( argc, argv, short_options, long_options, &option_index ) ;

    if (c == -1)
      break ;

    if (c == 'x') // reference genome file
    {
      idxPrefix = strdup(optarg) ;
    }
    else if (c == 'u')
    {
      reads.AddReadFile(optarg, false) ;
    }
    else if (c == '1')
    {
      reads.AddReadFile(optarg, true) ;
      hasMate = true ;
    }
    else if (c == '2')
    {
      mateReads.AddReadFile( optarg, true ) ;
      hasMate = true ;
    }
    else if (c == 'o')
    {
      strcpy(outputPrefix, optarg) ;
    }
    else if (c == 't')
    {
      threadCnt = atoi(optarg) ;
    }
    else if (c == 'k')
    {
      classifierParam.maxResult = atoi(optarg) ;
    }
    else if (c == 'v')
    {
      printf("Centrifuger v" CENTRIFUGER_VERSION "\n") ;
      exit(0) ; 
    }
    else if (c == 'm')
    {
      mergeReadPair = true ;
    }
    else if (c == ARGV_MIN_HITLEN)
    {
      classifierParam.minHitLen = atoi(optarg) ;
    }
    else if (c == ARGV_MAX_RESULT_PER_HIT_FACTOR)
    {
      classifierParam.maxResultPerHitFactor = atoi(optarg) ;
    }
    else if (c == ARGV_MERGE_READ_PAIR)
    {
      mergeReadPair = true ;
    }
    else if (c == ARGV_BARCODE)
    {
      hasBarcode = true ;
      barcodeFile.AddReadFile(optarg, false) ;
    }
    else if (c == ARGV_UMI)
    {
      hasUmi = true ;
      umiFile.AddReadFile(optarg, false) ;
    }
    else if (c == ARGV_READFORMAT)
    {
      readFormatter.Init(optarg) ;
    }
    else if (c == ARGV_BARCODE_WHITELIST)
    {
      barcodeCorrector.SetWhitelist(optarg) ;
      hasBarcodeWhitelist = true ;
    }
    else if (c == ARGV_BARCODE_TRANSLATE)
    {
      barcodeTranslator.SetTranslateTable(optarg) ;
    }
    else
    {
      Utils::PrintLog("Unknown parameter found.\n%s", usage ) ;
      return EXIT_FAILURE ;
    }
  }

  Utils::PrintLog("Centrifuger starts." ) ;
  if (idxPrefix == NULL)
  {
    Utils::PrintLog("Need to use -x to specify index prefix.") ;
    return EXIT_FAILURE ;
  }

  if ( hasBarcode && hasBarcodeWhitelist )
  {
    barcodeCorrector.CollectBackgroundDistribution(barcodeFile, readFormatter) ;
  }
  
  if (threadCnt > 1 && readFormatter.GetTotalSegmentCount() > 0)
    readFormatter.AllocateBuffers(4 * threadCnt) ;

  classifier.Init(idxPrefix, classifierParam) ;
  resWriter.SetHasBarcode(hasBarcode) ;
  resWriter.SetHasUmi(hasUmi) ;
  resWriter.OutputHeader() ;

  const int maxBatchSize = 1024 * threadCnt ;
  int batchSize ;
  
  int useInputThread = 0 ;
  int useOutputThread = 0 ;

  int classificationThreadCnt = threadCnt ;
  if (threadCnt > 7)
    useInputThread = 1 ;
  if (threadCnt > 12)
    useOutputThread = 1 ;

  classificationThreadCnt = threadCnt - useInputThread - useOutputThread ;

  pthread_t *threads = (pthread_t *)malloc( sizeof( pthread_t ) * classificationThreadCnt ) ;
  struct _threadArg *args = (struct _threadArg *)malloc( sizeof( struct _threadArg ) * classificationThreadCnt ) ;
  pthread_attr_t attr ;
  pthread_attr_init( &attr ) ;
  pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE ) ;
 
  for (i = 0 ; i < classificationThreadCnt ; ++i)
  {
    args[i].threadCnt = classificationThreadCnt ;
    args[i].tid = i ;
    args[i].classifier = &classifier ;
    args[i].readPairMerger = mergeReadPair ? &readPairMerger : NULL ;
  }

  //useLoadOutputThreads = false ;
  if (!useInputThread && !useOutputThread)
  {
    struct _Read *readBatch = NULL, *readBatch2 = NULL, *barcodeBatch = NULL, *umiBatch = NULL ;
    readBatch = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
    if ( hasMate )
      readBatch2 = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
    if ( hasBarcode )
      barcodeBatch = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
    if ( hasUmi )
      umiBatch = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
    
    struct _classifierResult *classifierBatchResults = new struct _classifierResult[maxBatchSize] ;
    
    for ( i = 0 ; i < classificationThreadCnt ; ++i )
    {
      args[i].readBatch = readBatch ;
      args[i].readBatch2 = readBatch2 ;
      args[i].results = classifierBatchResults ;
    }
    
    while ( 1 )
    {
      batchSize = GetReadBatch(reads, readBatch, mateReads, readBatch2, 
          barcodeFile, barcodeBatch, umiFile, umiBatch,
          readFormatter, barcodeCorrector, barcodeTranslator, maxBatchSize) ;

      if ( batchSize == 0 )
        break ; 

      for ( i = 0 ; i < classificationThreadCnt ; ++i )
      {
        args[i].batchSize = batchSize ;
        pthread_create( &threads[i], &attr, ClassifyReads_Thread, (void *)&args[i] ) ;
      }

      for ( i = 0 ; i < classificationThreadCnt ; ++i )
        pthread_join( threads[i], NULL ) ;

      for (i = 0 ; i < batchSize ; ++i)
        resWriter.Output(readBatch[i].id, hasBarcode ? barcodeBatch[i].seq : NULL,
            hasUmi ? umiBatch[i].seq : NULL, classifierBatchResults[i]) ;
    }
    
    reads.FreeBatch(readBatch, maxBatchSize) ;
    free(readBatch) ;
    if (hasMate)
    {
      mateReads.FreeBatch(readBatch2, maxBatchSize) ;
      free(readBatch2) ;
    }
    if (hasBarcode)
    {
      barcodeFile.FreeBatch(barcodeBatch, maxBatchSize) ;
      free(barcodeBatch) ;
    }
    if (hasUmi)
    {
      umiFile.FreeBatch(umiBatch, maxBatchSize) ;
      free(umiBatch) ;
    }
    delete[] classifierBatchResults ;
  }
  else if (useInputThread == 1 && useOutputThread == 0)
  {
    pthread_t inputThread ;
    struct _inputThreadArg inputThreadArg ;

    struct _Read *readBatch[2] ;
    struct _Read *readBatch2[2] ;
    struct _Read *barcodeBatch[2] ;
    struct _Read *umiBatch[2] ;
    struct _classifierResult *classifierBatchResults[2] ;
    
    for (i = 0 ; i < 2 ; ++i)
    {
      readBatch[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      if ( hasMate )
        readBatch2[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      else
        readBatch2[i] = NULL ;
      
      if ( hasBarcode )
        barcodeBatch[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      else
        barcodeBatch[i] = NULL ;

      if ( hasUmi )
        umiBatch[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      else
        umiBatch[i] = NULL ;

      classifierBatchResults[i] = new struct _classifierResult[maxBatchSize] ;
    }
    int batchSize[2] ;
    
    bool started = false ;
    // Load in the first batch
    batchSize[0] = GetReadBatch(reads, readBatch[0], mateReads, readBatch2[0], 
        barcodeFile, barcodeBatch[0], umiFile, umiBatch[0],
        readFormatter, barcodeCorrector, barcodeTranslator, maxBatchSize) ;
    
    int tag = 0 ; // which batch to use
    inputThreadArg.reads = &reads ;
    inputThreadArg.mateReads = &mateReads ;
    inputThreadArg.barcodeFile = &barcodeFile ;
    inputThreadArg.umiFile = &umiFile ;
    inputThreadArg.readFormatter = &readFormatter ;
    inputThreadArg.barcodeCorrector = &barcodeCorrector ;
    inputThreadArg.barcodeTranslator = &barcodeTranslator ;
    inputThreadArg.maxBatchSize = maxBatchSize ;

    while (1)
    {
      int nextTag = 1 - tag ;

      if (started)
        pthread_join(inputThread, NULL) ;

      if (batchSize[tag] == 0)
        break ;

      // Load in the next batch
      inputThreadArg.readBatch = readBatch[nextTag] ;
      inputThreadArg.readBatch2 = readBatch2[nextTag] ;
      inputThreadArg.barcodeBatch = barcodeBatch[nextTag] ;
      inputThreadArg.umiBatch = umiBatch[nextTag] ;
      inputThreadArg.pBatchSize = &batchSize[nextTag] ;
      pthread_create(&inputThread, &attr, LoadReads_Thread, (void *)&inputThreadArg) ;

      // Process the current batch
      for ( i = 0 ; i < classificationThreadCnt ; ++i )
      {
        args[i].readBatch = readBatch[tag] ;
        args[i].readBatch2 = readBatch2[tag] ;
        //args[i].barcodeBatch = barcodeBatch[tag] ;
        //args[i].umiBatch = umiBatch[tag] ;
        args[i].results = classifierBatchResults[tag] ;
        args[i].batchSize = batchSize[tag] ;

        pthread_create( &threads[i], &attr, ClassifyReads_Thread, (void *)&args[i] ) ;
      }

      for (i = 0 ; i < classificationThreadCnt ; ++i)
        pthread_join(threads[i], NULL) ;

      for (i = 0 ; i < batchSize[tag] ; ++i)
        resWriter.Output(readBatch[tag][i].id, hasBarcode ? barcodeBatch[tag][i].seq : NULL,
            hasUmi ? umiBatch[tag][i].seq : NULL, classifierBatchResults[tag][i]) ;

      started = true ;
      tag = nextTag ;
    }

    for (i = 0 ; i < 2 ; ++i)
    {
      reads.FreeBatch(readBatch[i], maxBatchSize) ;
      free(readBatch[i]) ;
      if (hasMate)
      {
        mateReads.FreeBatch(readBatch2[i], maxBatchSize) ;
        free(readBatch2[i]) ;
      }
      if (hasBarcode)
      {
        barcodeFile.FreeBatch(barcodeBatch[i], maxBatchSize) ;
        free(barcodeBatch[i]) ;
      }
      if (hasUmi)
      {
        umiFile.FreeBatch(umiBatch[i], maxBatchSize) ;
        free(umiBatch[i]) ;
      }
      delete[] classifierBatchResults[i] ;
    }
  }
  else //use both input and output thread
  {
    pthread_t inputThread ;
    struct _inputThreadArg inputThreadArg ;

    struct _Read *readBatch[3] ;
    struct _Read *readBatch2[3] ;
    struct _Read *barcodeBatch[3] ;
    struct _Read *umiBatch[3] ;
    struct _classifierResult *classifierBatchResults[3] ;
    
    for (i = 0 ; i < 3 ; ++i)
    {
      readBatch[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      if ( hasMate )
        readBatch2[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      else
        readBatch2[i] = NULL ;

      if ( hasBarcode )
        barcodeBatch[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      else
        barcodeBatch[i] = NULL ;

      if ( hasUmi )
        umiBatch[i] = ( struct _Read *)calloc( sizeof( struct _Read ), maxBatchSize ) ;
      else
        umiBatch[i] = NULL ;
      
      classifierBatchResults[i] = new struct _classifierResult[maxBatchSize] ;
    }
    int batchSize[3] ;
    
    bool started = false ;
    // Load in the first batch
    batchSize[0] = GetReadBatch(reads, readBatch[0], mateReads, readBatch2[0], 
        barcodeFile, barcodeBatch[0], umiFile, umiBatch[0],
        readFormatter, barcodeCorrector, barcodeTranslator, maxBatchSize) ;
    
    int tag = 0 ; // which batch to use
    inputThreadArg.reads = &reads ;
    inputThreadArg.mateReads = &mateReads ;
    inputThreadArg.barcodeFile = &barcodeFile ;
    inputThreadArg.umiFile = &umiFile ;
    inputThreadArg.readFormatter = &readFormatter ;
    inputThreadArg.barcodeCorrector = &barcodeCorrector ;
    inputThreadArg.barcodeTranslator = &barcodeTranslator ;
    inputThreadArg.maxBatchSize = maxBatchSize ;

    while (1)
    {
      int nextTag = (tag + 1) % 3 ;
      int prevTag = tag >= 1 ? tag - 1 : 2 ;
        
      if (started)
        pthread_join(inputThread, NULL) ;
      if (batchSize[tag] > 0)
      {
        // Load in the next batch
        inputThreadArg.readBatch = readBatch[nextTag] ;
        inputThreadArg.readBatch2 = readBatch2[nextTag] ;
        inputThreadArg.barcodeBatch = barcodeBatch[nextTag] ;
        inputThreadArg.umiBatch = umiBatch[nextTag] ;
        inputThreadArg.pBatchSize = &batchSize[nextTag] ;
        pthread_create(&inputThread, &attr, LoadReads_Thread, (void *)&inputThreadArg) ;
      }

      // Process the current batch
      if (started)
      {
        for (i = 0 ; i < classificationThreadCnt ; ++i)
          pthread_join(threads[i], NULL) ;
      }
      
      if (batchSize[tag] > 0)
      {
        for ( i = 0 ; i < classificationThreadCnt ; ++i )
        {
          args[i].readBatch = readBatch[tag] ;
          args[i].readBatch2 = readBatch2[tag] ;
          args[i].results = classifierBatchResults[tag] ;
          args[i].batchSize = batchSize[tag] ;

          pthread_create( &threads[i], &attr, ClassifyReads_Thread, (void *)&args[i] ) ;
        }
      }

      // Output the previous batch
      if (started)
      {
        for (i = 0 ; i < batchSize[prevTag] ; ++i)
          resWriter.Output(readBatch[prevTag][i].id, hasBarcode ? barcodeBatch[prevTag][i].seq : NULL,
              hasUmi ? umiBatch[prevTag][i].seq : NULL, classifierBatchResults[prevTag][i]) ;
      }
      
      if (batchSize[tag] == 0)
        break ;
      
      started = true ;
      tag = nextTag ;
    }

    for (i = 0 ; i < 3 ; ++i)
    {
      reads.FreeBatch(readBatch[i], maxBatchSize) ;
      free(readBatch[i]) ;
      if (hasMate)
      {
        mateReads.FreeBatch(readBatch2[i], maxBatchSize) ;
        free(readBatch2[i]) ;
      }
      if (hasBarcode)
      {
        barcodeFile.FreeBatch(barcodeBatch[i], maxBatchSize) ;
        free(barcodeBatch[i]) ;
      }
      if (hasUmi)
      {
        umiFile.FreeBatch(umiBatch[i], maxBatchSize) ;
        free(umiBatch[i]) ;
      }
      delete[] classifierBatchResults[i] ;
    }
  } // end of if-else for use input output thread
  
  pthread_attr_destroy( &attr ) ;
  free( threads ) ;
  free( args ) ;
  free(idxPrefix) ;

  Utils::PrintLog("Centrifuger finishes." ) ;
  return 0 ;
}
