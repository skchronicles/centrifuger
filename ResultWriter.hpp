#ifndef _MOURISL_RESULTWRITER_HEADER 
#define _MOURISL_RESULTWRITER_HEADER 

#include "Classifier.hpp" 

#include "ReadFormatter.hpp"
#include "BarcodeCorrector.hpp"
#include "BarcodeTranslator.hpp"

class ResultWriter
{
private:
  FILE *_fpClassification ;
  bool _hasBarcode ;
  bool _hasUmi ;

  void PrintExtraCol(const char *s) 
  {
    if (s == NULL)
      fprintf(_fpClassification, "\t") ;
    else
      fprintf(_fpClassification, "\t%s", s) ;
  }
public:
  //ResultWriter(const Taxonomy taxonomy): _taxonomy(taxonomy)  
  ResultWriter() 
  {
    _fpClassification = stdout ;
    _hasBarcode = false ;
    _hasUmi = false ;
  }

  ~ResultWriter() 
  {
    if (_fpClassification != stdout)
      fclose(_fpClassification) ;
  }
  
  void SetClassificationOutput(const char *filename)
  {
    _fpClassification = fopen(filename, "w") ;
  }

  void SetHasBarcode(bool s)
  {
    _hasBarcode = s ;
  }

  void SetHasUmi(bool s) 
  {
    _hasUmi = s ;
  }

  void OutputHeader()
  {
    fprintf(_fpClassification, "readID\tseqID\ttaxID\tscore\t2ndBestScore\thitLength\tqueryLength\tnumMatches") ;
  
    if (_hasBarcode)
      fprintf(_fpClassification, "\tbarcode") ;
    if (_hasUmi)
      fprintf(_fpClassification, "\tUMI") ;
    fprintf(_fpClassification, "\n") ;
  }

  void Output(const char *readid, const char *barcode, const char *umi, const struct _classifierResult &r)
  {
    int i ;
    int matchCnt = r.taxIds.size() ;
    if (matchCnt > 0)
    {
      for (i = 0 ; i < matchCnt ; ++i)
      {
        fprintf(_fpClassification,
            "%s\t%s\t%lu\t%lu\t%lu\t%d\t%d\t%d",
            readid, r.seqStrNames[i].c_str(), r.taxIds[i],
            r.score, r.secondaryScore, r.hitLength, r.queryLength, matchCnt) ;
        if (_hasBarcode)
          PrintExtraCol(barcode) ;
        if (_hasUmi)
          PrintExtraCol(umi) ;
      }
    }
    else
    {
      fprintf(_fpClassification,
          "%s\tunclassified\t0\t0\t0\t0\t%d\t1", readid, r.queryLength) ;
      if (_hasBarcode)
        PrintExtraCol(barcode) ;
      if (_hasUmi)
        PrintExtraCol(umi) ;
    }
  }

  // Another type of output that requires extract barcode and umi information.
  void Output(char *readid, char *seq, char *qual, ReadFormatter &readFormatter, BarcodeCorrector &barcodeCorrector, BarcodeTranslator &barcodeTranslator, const struct _classifierResult &r)
  {
    char *barcode = NULL ;
    char *barcodeQual = NULL ;
    char *umi = NULL ;
      
    if (_hasBarcode)
    {
      barcode = readFormatter.Extract(seq, FORMAT_BARCODE, true, true, 0) ;
      barcodeQual = readFormatter.Extract(seq, FORMAT_BARCODE, false, true, 1) ;
    }
    if (_hasUmi)
      umi = readFormatter.Extract(seq, FORMAT_BARCODE, true, true, 2) ;
    if (barcodeCorrector.GetWhitelistSize() > 0)
      barcodeCorrector.Correct(barcode, barcodeQual) ;
    
    Output(readid, barcodeTranslator.IsSet() ? barcodeTranslator.Translate(barcode, strlen(barcode)).c_str() : barcode,
        umi, r) ;
  }

  void Finalize()
  {
  }
} ;

#endif
