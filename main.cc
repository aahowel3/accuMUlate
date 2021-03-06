#include <iostream>
#include <stdint.h>
#include <map>
#include <vector>
#include <string>
#include <sys/stat.h>



#include "boost/program_options.hpp"
#include "api/BamReader.h"
#include "utils/bamtools_pileup_engine.h"
#include "utils/bamtools_fasta.h"

#include "model.h"
#include "parsers.h"

using namespace std;
using namespace BamTools;

                             
class VariantVisitor : public PileupVisitor{
    public:
        VariantVisitor(const RefVector& bam_references, 
                       const SamHeader& header,
                       const Fasta& idx_ref,
                       ostream *out_stream,
                       const SampleMap& samples, 
                       const ModelParams& p,  
                       BamAlignment& ali, 
                       int qual_cut,
                       int mapping_cut,
                       double prob_cut):

            PileupVisitor(), m_idx_ref(idx_ref), m_bam_ref(bam_references), 
                             m_header(header), m_samples(samples), 
                             m_qual_cut(qual_cut), m_params(p), m_ali(ali), 
                             m_ostream(out_stream), m_prob_cut(prob_cut),
                             m_mapping_cut(mapping_cut)
                              { }
        ~VariantVisitor(void) { }
    public:
         void Visit(const PileupPosition& pileupData) {
             uint64_t pos  = pileupData.Position;
             m_idx_ref.GetBase(pileupData.RefId, pos, current_base);
             ReadDataVector bcalls (m_samples.size(), ReadData{{ 0,0,0,0 }}); 
             for(auto it = begin(pileupData.PileupAlignments);
                      it !=  end(pileupData.PileupAlignments); 
                      ++it){
                 if( include_site(*it, m_mapping_cut, m_qual_cut) ){
                    it->Alignment.GetTag("RG", tag_id);
                    uint32_t sindex = m_samples[tag_id]; //TODO check samples existed! 
                    uint16_t bindex  = base_index(it->Alignment.QueryBases[it->PositionInAlignment]);
                    if (bindex < 4 ){
                        bcalls[sindex].reads[bindex] += 1;
                    }
                }
            }
            uint16_t ref_base_idx = base_index(current_base);
            if (ref_base_idx < 4  ){ //TODO Model for bases at which reference is 'N' (=masked for Tt, maybe not others?)
                ModelInput d = {ref_base_idx, bcalls};
                double prob_one = TetMAProbOneMutation(m_params,d);
                double prob = TetMAProbability(m_params, d);
                if(prob >= m_prob_cut){
                     *m_ostream << m_bam_ref[pileupData.RefId].RefName << '\t'
                                << pos << '\t' 
                                << current_base << '\t' 
                                << prob << '\t' 
                                << prob_one << '\t' 
                                << endl;          
                }
            }
         }
    private:
        RefVector m_bam_ref;
        SamHeader m_header;
        Fasta m_idx_ref; 
        ostream* m_ostream;
        SampleMap m_samples;
        BamAlignment& m_ali;
        ModelParams m_params;
        int m_qual_cut;
        int m_mapping_cut;
        double m_prob_cut;
        char current_base;
        string tag_id;
        uint64_t chr_index;
};



int main(int argc, char** argv){

    namespace po = boost::program_options;
    string ref_file;
    string config_path;
    po::options_description cmd("Command line options");
    cmd.add_options()
        ("help,h", "Print a help message")
        ("bam,b", po::value<string>()->required(), "Path to BAM file")
        ("bam-index,x", po::value<string>()->default_value(""), "Path to BAM index, (defalult is <bam_path>.bai")
        ("reference,r", po::value<string>(&ref_file)->required(),  "Path to reference genome")
//       ("ancestor,a", po::value<string>(&anc_tag), "Ancestor RG sample ID")
//        ("sample-name,s", po::value<vector <string> >()->required(), "Sample tags")
        ("qual,q", po::value<int>()->default_value(13), 
                   "Base quality cuttoff")
        
        ("mapping-qual,m", po::value<int>()->default_value(13), 
                    "Mapping quality cuttoff")
     
        ("prob,p", po::value<double>()->default_value(0.1),
                   "Mutaton probability cut-off")
        ("out,o", po::value<string>()->default_value("acuMUlate_result.tsv"),
                    "Out file name")
        ("intervals,i", po::value<string>(), "Path to bed file")
        ("config,c", po::value<string>(), "Path to config file")
        ("theta", po::value<double>()->required(), "theta")            
        ("nfreqs", po::value<vector<double> >()->multitoken(), "")     
        ("mu", po::value<double>()->required(), "")  
        ("seq-error", po::value<double>()->required(), "") 
        ("phi-haploid",     po::value<double>()->required(), "") 
        ("phi-diploid",     po::value<double>()->required(), ""); 

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, cmd), vm);

    if (vm.count("help")){
        cout << cmd << endl;
        return 0;
    }

    if (vm.count("config")){
        ifstream config_stream (vm["config"].as<string>());
        po::store(po::parse_config_file(config_stream, cmd, false), vm);
    }

    vm.notify();
    ModelParams params = {
        vm["theta"].as<double>(),
        vm["nfreqs"].as<vector< double> >(),
        vm["mu"].as<double>(),
        vm["seq-error"].as<double>(), 
        vm["phi-haploid"].as<double>(), 
        vm["phi-diploid"].as<double>(),
    };
    string bam_path = vm["bam"].as<string>();
    string index_path = vm["bam-index"].as<string>();
    if(index_path == ""){
        index_path = bam_path + ".bai";
    }   

    ofstream result_stream (vm["out"].as<string>());
    // Start setiing up files
    //TODO: check sucsess of all these opens/reads:

    BamReader experiment; 
    experiment.Open(bam_path);
    experiment.OpenIndex(index_path);
    RefVector references = experiment.GetReferenceData(); 
    SamHeader header = experiment.GetHeader();

    
    //Fasta reference
    Fasta reference_genome; // BamTools::Fasta
    struct stat file_info;
    string faidx_path = ref_file + ".fai";
    if (stat(faidx_path.c_str(), &file_info) != 0){
        reference_genome.Open(ref_file);
        reference_genome.CreateIndex(faidx_path);
    }
    reference_genome.Open(ref_file, faidx_path);

    // Map readgroups to samples
    // TODO: this presumes first sample is ancestor. True for our data, not for
    // others.
    // First map all sample names to an index for ReadDataVectors
    SampleMap name_map;
    uint16_t sindex = 0;
    for(auto it = header.ReadGroups.Begin(); it!= header.ReadGroups.End(); it++){
        if(it->HasSample()){
            auto s  = name_map.find(it->Sample);
            if( s == name_map.end()){ // not in there yet
                name_map[it->Sample] = sindex;
                sindex += 1;
            }
        }
    }
    // And now, go back over the read groups to map RG:sample index
    SampleMap samples;
    for(auto it = header.ReadGroups.Begin(); it!= header.ReadGroups.End(); it++){
        if(it->HasSample()){
            samples[it->ID] = name_map[it->Sample];  
        }
    }

    PileupEngine pileup;
    BamAlignment ali;

    VariantVisitor *v = new VariantVisitor(
            references,
            header,
            reference_genome, 
            &result_stream,
//            vm["sample-name"].as<vector< string> >(),
            samples,
            params, 
            ali, 
            vm["qual"].as<int>(), 
            vm["mapping-qual"].as<int>(),
            vm["prob"].as<double>()
        );
    pileup.AddVisitor(v);
   
    if (vm.count("intervals")){
        BedFile bed (vm["intervals"].as<string>());
        BedInterval region;
        while(bed.get_interval(region) == 0){
            int ref_id = experiment.GetReferenceID(region.chr);
            experiment.SetRegion(ref_id, region.start, ref_id, region.end);
            while( experiment.GetNextAlignment(ali) ){
                pileup.AddAlignment(ali);
            }
        }
    }
    else{
        while( experiment.GetNextAlignment(ali)){
            pileup.AddAlignment(ali);
        }  
    }
    pileup.Flush();
    return 0;
}


