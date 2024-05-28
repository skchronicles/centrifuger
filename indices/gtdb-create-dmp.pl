#!/bin/perl

use strict ;
use warnings ;

use Cwd qw(abs_path) ;
use Getopt::Long ;
use threads ;
use threads::shared ;

# Create the dump files that are necessary for centrifuger-build from GTDB files
sub AccessionToSubdir
{
  my $accession = $_[0] ;
  return substr($accession, 0, 3)."/".substr($accession, 4, 3).
        "/".substr($accession, 7, 3)."/".substr($accession, 10, 3) ;
}

sub GetGenomeFilePath
{
  my $dir = $_[0] ; 
  my $accession = $_[1] ; #GCF_000657795.2
  my $subdir = AccessionToSubdir($accession)  ;

  return $dir."/database/".$subdir."/".$accession."_genomic.fna.gz" ;
}

sub PrintLog
{
  print STDERR "[".localtime()."] ".$_[0]."\n" ;
}


my $usage = "Usage: create-dmp-gtdb.pl [OPTIONS]\n".
  "\t-d STR: directory of GTDB decompressed sequence\n".
  "\t-m STR: GTDB metadata file\n".
  "\t-o STR: output prefix [gtdb_]\n".
  "\t-t INT: number of threads to use [1]\n".
  "\t--names STR: NCBI's names.dmp file. If not given, using non-NCBI taxid to represent intermediate nodes.\n".
  "\t--skipSeqId2TaxId: skip the step of generating seqid_to_taxid.map file.\n"
  ;

die "$usage\n" if (@ARGV == 0) ;

# Generate names.dmp and nodes.dmp files using metadata file
PrintLog("Generate the dmp files for nodes and names, and the mapping from file path to tax ID.") ;

my $ncbiNodeDmp = "" ;
my $ncbiNameDmp = "" ;
my $outputPrefix = "gtdb" ;
my $genomeDir = "" ;
my $metaFile = "" ;
my $novelTaxId = 10000000 ;
my $numThreads = 1 ;
my $skipSeqIdMap = 0 ;

GetOptions(
  "o=s" => \$outputPrefix,
  "d=s" => \$genomeDir,
  "m=s" => \$metaFile,
  "t=i" => \$numThreads, 
  #"nodes=s" => \$ncbiNodeDmp,
  "names=s" => \$ncbiNameDmp,
  "skipSeqId2TaxId" => \$skipSeqIdMap 
) ;
my $fullGenomeDir = abs_path($genomeDir) ;

my $i ;
my @cols ;

my %taxRankCodeToFull = ("d"=>"domain", "p"=>"phylum", "c"=>"class",
         "o"=>"order", "f"=>"family", "g"=>"genus", "s"=>"species", "x"=>"no rank" ) ;

# Collect the NCBI names information
#3041336	|	Stipitochalara	|		|	scientific name	|
my %ncbiNamesToTaxId ;
if ($ncbiNameDmp ne "")
{
  open FP, $ncbiNameDmp ;
  while (<FP>)
  {
    chomp ;
    @cols = split /\t/ ;
    next if ($cols[6] ne "scientific name") ;

    my $name = $cols[2] ;
    $name =~ s/\s/_/g ;
    $ncbiNamesToTaxId{$name} = $cols[0] ;
  }
  close FP ;
}


open FPoutNames, ">${outputPrefix}_names.dmp" ;
open FPoutNodes, ">${outputPrefix}_nodes.dmp" ;
open FPoutFileToTaxid, ">${outputPrefix}_fname_to_taxid.map" ;
open FPoutFileList, ">${outputPrefix}_file.list" ;
open FPmeta, $metaFile ;

print FPoutNodes "1\t|\t1\t|\tno rank\t|\n" ;
print FPoutNames "1\t|\troot\t|\tscientific name\t|\n" ;

my $header = <FPmeta> ;
my %colNames ; 

@cols = split /\t/, $header ; 
for ($i = 0 ; $i < scalar(@cols) ; ++$i)
{
  $colNames{ $cols[$i] } = $i ;
}

my @fileNames ;
my %accessionToTaxId ;
my %nodesToPrint ;
my %taxIdRank ;
my %namesToPrint ;

while (<FPmeta>)
{
  @cols = split /\t/ ;
  next if ($cols[ $colNames{"gtdb_representative"}] ne "t") ;
  
  my $accession = substr($cols[ $colNames{"accession"} ], 3) ;

  #if (! -e GetGenomeFilePath($genomeDir, $accession) )
  #{
  #  print("Warning: $accession not exists\n") ;
  #  next ;
  #}

  my $taxid = $cols[ $colNames{"ncbi_taxid"} ] ; 
  my $lineage = $cols[ $colNames{"gtdb_taxonomy"} ] ;
  
  next if ($taxid eq "none") ;

  #d__Bacteria;p__Pseudomonadota;c__Gammaproteobacteria;o__Burkholderiales;f__Burkholderiaceae;g__Bordetella;s__Bordetella pseudohinzii
  my @lineageFields = split /;/, $lineage ;

  my $j ;
  my $parentTid = 1 ;
  my $ltid = 1 ;
  for ($j = 0 ; $j < scalar(@lineageFields) ; ++$j)
  {
    my @cols2 = split /__/, $lineageFields[$j] ;

    if (defined $ncbiNamesToTaxId{$cols2[1]} && $j < scalar(@lineageFields) - 1)
    {
      $ltid = $ncbiNamesToTaxId{$cols2[1]} ;
    }
    elsif ($j == scalar(@lineageFields) - 1)
    {
      $ltid = $taxid ;
    }
    else
    {
      $ltid = $novelTaxId ;
      $ncbiNamesToTaxId{$cols2[1]} = $ltid ;
      ++$novelTaxId ;
    }

    $nodesToPrint{$ltid} = $parentTid ;
    $taxIdRank{$ltid} = $cols2[0] ;
    $namesToPrint{$ltid} = $cols2[1]  ;

    $parentTid = $ltid ;
  }

  $accessionToTaxId{$accession} = $taxid ;
  print FPoutFileToTaxid GetGenomeFilePath($fullGenomeDir, $accession)."\t$taxid\n" ;
  print FPoutFileList GetGenomeFilePath($fullGenomeDir, $accession)."\n" ;
}

foreach my $tid (keys %nodesToPrint)
{
  print FPoutNodes "$tid\t|\t".$nodesToPrint{$tid}."\t|\t".$taxRankCodeToFull{$taxIdRank{$tid}}."\t|\n" ;
  print FPoutNames "$tid\t|\t".$namesToPrint{$tid}."\t|\tscientific name\t|\n" ;
}

close FPmeta ;
close FPoutNames ;
close FPoutNodes ;
close FPoutFileToTaxid ;
close FPoutFileList ;

# Iterate through the genome files to generate the seqid map file
if ($skipSeqIdMap)
{
  exit(0) ;
}

PrintLog("Generate the seq ID to tax ID mapping file.") ;
my %seqIdMap ;
my @threadSeqIdMap : shared;
my @threads ;

for ( $i = 0 ; $i < $numThreads ; ++$i )
{
	push @threads, $i ;
  my %tmp : shared ;
  push @threadSeqIdMap, \%tmp ;
}

sub GetSeqIdMapThread
{
  my $tid = threads->tid() - 1 ;
  foreach my $accession (keys %accessionToTaxId)
  {
    my $tmp = int(substr($accession, 4) / 10) ; # seems many accession number are ending with 5, so there are definitely some biases.
    if ($tmp % $numThreads == $tid)
    {
      my $file = GetGenomeFilePath($genomeDir, $accession) ;
      system("gzip -cd < $file | grep '^>' > ${outputPrefix}_tmp_thread_${tid}.tmp") ;
      open FP, "${outputPrefix}_tmp_thread_${tid}.tmp" ;
      while (<FP>)
      {
        if (/^>/)
        {
          chomp ;
          my $seqId = substr((split /\s/, $_)[0], 1) ;
          ${$threadSeqIdMap[$tid]}{$seqId} = $accessionToTaxId{$accession} ;
        }
      }
      close FP ;
    }
  }
  unlink "${outputPrefix}_tmp_thread_${tid}.tmp" ;
}

foreach (@threads)
{
  $_ = threads->create(\&GetSeqIdMapThread) ;
}
foreach (@threads)
{
  $_->join() ;
}

foreach (@threadSeqIdMap)
{
  my %tmp = %{$_} ;
  foreach my $seqId (keys %tmp)
  {
    $seqIdMap{$seqId} = $tmp{$seqId} ;
  }
}

open FPout, ">${outputPrefix}_seqid_to_taxid.map" ;
foreach my $seqId (keys %seqIdMap)
{
  print FPout "$seqId\t".$seqIdMap{$seqId}."\n" ;
}
close FPout ;

PrintLog("Done.") ;
