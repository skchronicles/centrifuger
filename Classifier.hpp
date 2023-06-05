#ifndef _MOURISL_CLASSIFIER_HEADER
#define _MOURISL_CLASSIFIER_HEADER

#include <string.h>

#include "Taxonomy.hpp"
#include "compactds/FMIndex.hpp"
#include "compactds/Sequence_Hybrid.hpp"
#include "SimpleVector.hpp"

struct _classifierParam 
{
  int maxResult ; // the number of entries in the results    
  int minHitLen ;
  _classifierParam()
  {
    maxResult = 1 ;
    minHitLen = 22 ;
  }
} ;

struct _classifierResult
{
  size_t score ;
  int secondaryScore ;
  int bestMatchCnt ; // the number of matches for best match
  std::vector<size_t> seqHits ; // sequence id or taxonomy id
} ;

struct _BWTHit
{
  size_t sp, ep ; //[sp, ep] range on BWT 
  int l ; // hit length

  _BWTHit(size_t isp, size_t iep, int il)
  {
    sp = isp ;
    ep = iep ;
    l = il ;
  }
} ;

class Classifier
{
private:
  FMIndex<Sequence_Hybrid> _fm ;
  Taxonomy _taxonomy ;
  std::map<size_t, size_t> _seqLength ;
  _classifierParam _param ;
  int _scoreHitLenAdjust ;
  char _compChar[256] ;
  
  void ReverseComplement(char *r, int len)
  {
    int i, j ;
    for (i = 0, j = len - 1 ; i < j ; ++i)
    {
      char tmp ;
      tmp = r[i] ;
      r[i] = r[j] ;
      r[j] = tmp ;
    }
    for (i = 0 ; i < len ; ++i)
      r[i] = _compChar[(int)r[i]] ; 
  }

  // one hit
  size_t CalculateHitScore(struct _BWTHit hit)
  {
    if (hit.l < _param.minHitLen)
      return 0 ;
    return (hit.l - _scoreHitLenAdjust) * (hit.l - _scoreHitLenAdjust) ;
  }

  // hit list
  size_t CalculateHitsScore(const SimpleVector<struct _BWTHit> &hits)
  {
    int i ;
    int hitCnt = hits.Size() ;
    size_t score = 0 ;
    for (i = 0 ; i < hitCnt ; ++i)
    {
      score += CalculateHitScore(hits[i]) ; 
    }
    return score ;
  }

  //@return: the number of hits 
  size_t GetHitsFromRead(char *r, size_t len, SimpleVector<struct _BWTHit> &hits) 
  {
    size_t i ;
    size_t sp = 0, ep = 0, l = 0 ;
    int remaining = len ;
    
    while (remaining >= _param.minHitLen)
    {
      l = _fm.BackwardSearch(r, remaining, sp, ep) ;
      if (l >= _param.minHitLen)
      {
        struct _BWTHit nh(sp, ep, l) ;
        hits.PushBack(nh) ;
      }

      // +1 is to skip the base
      remaining -= (l + 1) ;
    }
    return hits.Size() ;
  }

  //@return: the size of the hits after selecting the strand 
  size_t SearchForwardAndReverse(char *r1, char *r2, SimpleVector<struct _BWTHit> &hits)
  {
    char *rcR1 = NULL ;
    char *rcR2 = NULL ;
    int r1len = strlen(r1) ;
    rcR1 = strdup(r1) ;
    ReverseComplement(rcR1, r1len) ;

    SimpleVector<struct _BWTHit> forwardHits, reverseHits ;
    
    GetHitsFromRead(r1, r1len, forwardHits) ;
    GetHitsFromRead(rcR1, r1len, reverseHits) ;

    if (r2)
    {
      rcR2 = strdup(r2) ;
      int r2len = strlen(r2) ;
      ReverseComplement(rcR2, r2len) ;
      GetHitsFromRead(rcR2, r2len, forwardHits) ;
      GetHitsFromRead(r2, r2len, reverseHits) ;
    }
    
    int forwardScore = CalculateHitsScore(forwardHits) ;
    int reverseScore = CalculateHitsScore(reverseHits) ;
    
    if (forwardScore >= reverseScore)
      hits = forwardHits ;
    else
      hits = reverseHits ;

    free(rcR1) ;
    if (rcR2)
      free(rcR2) ;

    return hits.Size() ;
  }

  size_t GetClassificationFromHits(const SimpleVector<struct _BWTHit> &hits, struct _classifierResult &result)
  {
    int i ;
    size_t j ;
    int hitCnt = hits.Size() ;
    std::map<size_t, size_t> seqIdScore ;
    for (i = 0 ; i < hitCnt ; ++i)
    {
      size_t score = CalculateHitScore(hits[i]) ;
      for (j = hits[i].sp ; j <= hits[i].ep ; ++j)
      {
        size_t backsearchL = 0 ;
        size_t seqId = _fm.BackwardToSampledSA(j, backsearchL) ;
        if (seqIdScore.find(seqId) == seqIdScore.end())
          seqIdScore[seqId] = score ;
        else
          seqIdScore[seqId] += score ;
      }
    }

    // Select the best score
    size_t bestScore = 0 ;
    size_t secondBestScore = 0 ;
    for (std::map<size_t, size_t>::iterator iter = seqIdScore.begin() ; 
        iter != seqIdScore.end() ; ++iter)
    {
      if (iter->second > bestScore)
      {
        secondBestScore = bestScore ;
        bestScore = iter->second ;
      }
      else if (iter->second > secondBestScore)
        secondBestScore = iter->second ;
    }

    result.score = bestScore ;
    result.secondaryScore = secondBestScore ;

    std::vector<size_t> bestSeqIds ;
    for (std::map<size_t, size_t>::iterator iter = seqIdScore.begin() ; 
        iter != seqIdScore.end() ; ++iter)
    {
      if (iter->second == bestScore)
        bestSeqIds.push_back(iter->first) ;
    }
    result.bestMatchCnt = bestSeqIds.size() ;
    //TODO: LCA if above user-specificed 
    result.seqHits = bestSeqIds ;
  }
  

public:
  Classifier() 
  {
    _scoreHitLenAdjust = 15 ;
    int i ;
    for (i = 0 ; i < 256 ; ++i)
      _compChar[i] = 'N' ;
    _compChar['A'] = 'T' ;
    _compChar['C'] = 'G' ;
    _compChar['G'] = 'C' ;
    _compChar['T'] = 'A' ;
  }

  ~Classifier() {Free() ;}

  void Free()
  {
    _fm.Free() ;
    _taxonomy.Free() ;
    _seqLength.clear() ;
  }

  void Init(char *idxPrefix, struct _classifierParam param)
  {
    FILE *fp ;
    char *nameBuffer = (char *)malloc(sizeof(char) * (strlen(idxPrefix) + 17))  ;  
 
    // .1.cfr file for FM index
    sprintf(nameBuffer, "%s.1.cfr", idxPrefix) ;
    fp = fopen(nameBuffer, "r") ;
    _fm.Load(fp) ;
    fclose(fp) ;

    // .2.cfr file is for taxonomy structure
    sprintf(nameBuffer, "%s.2.cfr", idxPrefix) ;
    fp = fopen(nameBuffer, "r") ;
    _taxonomy.Load(fp) ;
    fclose(fp) ;

    // .3.cfr file is for sequence length
    sprintf(nameBuffer, "%s.3.cfr", idxPrefix) ;
    fp = fopen(nameBuffer, "r") ;
    size_t tmp[2] ;
    while (fread(tmp, sizeof(tmp[0]), 2, fp))
    {
      _seqLength[tmp[0]] = tmp[1] ;
    }
    fclose(fp) ;
    
    _param = param ;

    free(nameBuffer) ;
  }

  // Main function to return the classification results 
  void Query(char *r1, char *r2, struct _classifierResult &result)
  {
    size_t i ;
    
    SimpleVector<struct _BWTHit> hits ;
    int hitCnt = SearchForwardAndReverse(r1, r2, hits) ;
    GetClassificationFromHits(hits, result) ;
  }
} ;

#endif 
