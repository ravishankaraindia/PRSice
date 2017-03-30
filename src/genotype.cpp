/*
 * genotype.cpp
 *
 *  Created on: 27 Mar 2017
 *      Author: shingwanchoi
 */

#include "genotype.hpp"

void Genotype::init_chr(int num_auto, bool no_x, bool no_y, bool no_xy, bool no_mt)
{
	// this initialize haploid mask as the maximum possible number
	m_haploid_mask = new uintptr_t[CHROM_MASK_WORDS];
	fill_ulong_zero(CHROM_MASK_WORDS, m_haploid_mask);

	if(num_auto < 0)
	{
		num_auto = -num_auto;
		m_autosome_ct = num_auto;
		m_xymt_codes[X_OFFSET] = -1;
		m_xymt_codes[Y_OFFSET] = -1;
		m_xymt_codes[XY_OFFSET] = -1;
		m_xymt_codes[MT_OFFSET] = -1;
		m_max_code = num_auto;
		fill_all_bits(((uint32_t)num_auto) + 1, m_haploid_mask);
	}
	else
	{
		m_autosome_ct = num_auto;
		m_xymt_codes[X_OFFSET] = num_auto+1;
		m_xymt_codes[Y_OFFSET] = num_auto+2;
		m_xymt_codes[XY_OFFSET] = num_auto+3;
		m_xymt_codes[MT_OFFSET] = num_auto+4;
		set_bit(num_auto + 1, m_haploid_mask);
		set_bit(num_auto + 2, m_haploid_mask);
		if(no_x){
			m_xymt_codes[X_OFFSET] = -1;
			clear_bit(num_auto + 1, m_haploid_mask);
		}
		if(no_y)
		{
			m_xymt_codes[Y_OFFSET] = -1;
			clear_bit(num_auto + 2, m_haploid_mask);
		}
		if(no_xy)
		{
			m_xymt_codes[XY_OFFSET] = -1;
		}
		if(no_mt)
		{
			m_xymt_codes[MT_OFFSET] = -1;
		}
		if (m_xymt_codes[MT_OFFSET] != -1) {
			m_max_code = num_auto + 4;
		} else if (m_xymt_codes[XY_OFFSET] != -1) {
			m_max_code = num_auto + 3;
		} else if (m_xymt_codes[Y_OFFSET] != -1) {
			m_max_code = num_auto + 2;
		} else if (m_xymt_codes[X_OFFSET] != -1) {
			m_max_code = num_auto + 1;
		} else {
			m_max_code = num_auto;
		}
	}
}

void Genotype::set_genotype_files(std::string prefix)
{
	if(prefix.find("#")!=std::string::npos)
	{
		for(size_t chr = 1; chr < m_max_code; ++chr)
		{
			std::string name = prefix;
			misc::replace_substring(name, "#", std::to_string(chr));
			m_genotype_files.push_back(name);
		}
	}
	else
	{
		m_genotype_files.push_back(prefix);
	}
}

Genotype::Genotype(std::string prefix, int num_auto,
		bool no_x, bool no_y, bool no_xy, bool no_mt, const size_t thread, bool verbose) {
	// TODO Auto-generated constructor stub
	init_chr(num_auto, no_x, no_y, no_xy, no_mt);
	// obtain files
	set_genotype_files(prefix);
	load_sample();
	m_existed_snps=load_snps();
	if(verbose)
	{
		fprintf(stderr, "%zu people (%zu males, %zu females) loaded from .fam\n", m_unfiltered_sample_ct, m_num_male, m_num_female);
		fprintf(stderr, "%zu variants included\n", m_marker_ct);
	}
}

Genotype::~Genotype() {
	// TODO Auto-generated destructor stub
	if(m_founder_info!=nullptr) delete [] m_founder_info;
	if(m_sex_male != nullptr) delete [] m_sex_male;
	if(m_sample_exclude != nullptr) delete [] m_sample_exclude;
	if(m_marker_exclude != nullptr) delete [] m_marker_exclude;
	if(m_haploid_mask != nullptr) delete [] m_haploid_mask;
}



double Genotype::update_existed( Genotype &reference)
{
	int miss_match = 0;
	int matched = 0;
	for(auto &&snp : m_existed_snps)
	{
		auto target =reference.m_existed_snps_index.find(snp.get_rs());
		if(target==reference.m_existed_snps_index.end())
		{
			snp.not_required();
		}
		else
		{
			matched ++;
			//soft check, no biggy if they are not the same
			miss_match+=(snp==m_existed_snps[target->second])? 0 : 1;
		}
	}
	return (matched==0)? -1 : (double)miss_match/matched;
}

double Genotype::update_existed(const std::unordered_map<std::string, int> &ref_index,
		const std::vector<SNP> &reference)
{
	int miss_match = 0;
	int matched = 0;
	for(auto &&snp : m_existed_snps)
	{
		auto target = ref_index.find(snp.get_rs());
		if(target==ref_index.end())
		{
			snp.not_required();
		}
		else
		{
			matched ++;
			//soft check, no biggy if they are not the same
			miss_match+=(snp==m_existed_snps[target->second])? 0 : 1;
		}
	}
	return (matched==0)? -1 : (double)miss_match/matched;
}

void Genotype::read_base(const Commander &c_commander, const Region &region)
{
	// can assume region is of the same order as m_existed_snp
	const std::string input = c_commander.base_name();
	const bool beta = c_commander.beta();
	const bool fastscore = c_commander.fastscore();
	const bool full = c_commander.full();
	std::vector<int> index = c_commander.index(); // more appropriate for commander
	// now coordinates obtained from target file instead. Coordinate information
	// in base file only use for validation
	std::ifstream snp_file;
	snp_file.open(input.c_str());
	if(!snp_file.is_open())
	{
		std::string error_message = "ERROR: Cannot open base file: " +input;
		throw std::runtime_error(error_message);
	}
	int max_index = index[+BASE_INDEX::MAX];
	std::string line;
	if (!c_commander.has_index()) std::getline(snp_file, line);

	// category related stuff
	double threshold = (c_commander.fastscore())? c_commander.bar_upper() : c_commander.upper();
	double bound_start = c_commander.lower();
	double bound_end = c_commander.upper();
	double bound_inter = c_commander.inter();

	threshold = (full)? 1.0 : threshold;
	std::vector < std::string > token;

	bool exclude = false;
	// Some QC countss
	size_t num_duplicated = 0;
	size_t num_excluded = 0;
	size_t num_not_found = 0;
	size_t num_mismatched = 0;
	size_t num_not_converted = 0; // this is for NA
	size_t num_negative_stat = 0;
	std::unordered_set<std::string> dup_index;
	// Actual reading the file, will do a bunch of QC
	while (std::getline(snp_file, line))
	{
		misc::trim(line);
		if (!line.empty())
		{
			exclude = false;
			token = misc::split(line);
			if (token.size() <= max_index)
				throw std::runtime_error("More index than column in data");
			else
			{
				std::string rs_id = token[index[+BASE_INDEX::RS]];
				auto target = m_existed_snps_index.find(rs_id);
				if(target!=m_existed_snps_index.end() && dup_index.find(rs_id)==dup_index.end())
				{
					dup_index.insert(rs_id);
					auto &cur_snp = m_existed_snps[target->second];
					if(!cur_snp.is_required()) num_excluded++;
					else
					{
						int32_t chr_code = -1;
						if (index[+BASE_INDEX::CHR] >= 0)
						{
							chr_code = get_chrom_code_raw(token[index[+BASE_INDEX::CHR]].c_str());
							if (((const uint32_t)chr_code) > m_max_code) {
								if (chr_code != -1) {
									if (chr_code >= MAX_POSSIBLE_CHROM) {
										chr_code= m_xymt_codes[chr_code - MAX_POSSIBLE_CHROM];
									}
									else
									{
										std::string error_message ="ERROR: Cannot parse chromosome code: "
												+ token[index[+BASE_INDEX::CHR]];
										throw std::runtime_error(error_message);
									}
								}
							}
						}
						std::string ref_allele = (index[+BASE_INDEX::REF] >= 0) ? token[index[+BASE_INDEX::REF]] : "";
						std::string alt_allele = (index[+BASE_INDEX::ALT] >= 0) ? token[index[+BASE_INDEX::ALT]] : "";
						int loc = -1;
						if (index[+BASE_INDEX::BP] >= 0)
						{
							// obtain the SNP coordinate
							try {
								loc = misc::convert<int>( token[index[+BASE_INDEX::BP]].c_str());
								if (loc < 0)
								{
									std::string error_message = "ERROR: "+rs_id+" has negative loci!\n";
									throw std::runtime_error(error_message);
								}
							} catch (const std::runtime_error &error) {
								std::string error_message = "ERROR: Non-numeric loci for "+rs_id+"!\n";
								throw std::runtime_error(error_message);
							}
						}
						bool flipped = false;
						if(!cur_snp.matching(chr_code, loc, ref_allele, alt_allele, flipped))
						{
							num_mismatched++;
							exclude = true; // hard check, as we can't tell if that is correct or not anyway
						}
						double pvalue = 2.0;
						try{
							misc::convert<double>( token[index[+BASE_INDEX::P]]);
							if (pvalue < 0.0 || pvalue > 1.0)
							{
								std::string error_message = "ERROR: Invalid p-value for "+rs_id+"!\n";
								throw std::runtime_error(error_message);
							}
							else if (pvalue > threshold)
							{
								exclude = true;
								num_excluded++;
							}
						}catch (const std::runtime_error& error) {
							exclude = true;
							num_not_converted = true;
						}
						double stat = 0.0;
						try {
							stat = misc::convert<double>( token[index[+BASE_INDEX::STAT]]);
							if(stat <0 && !beta)
							{
								num_negative_stat++;
								exclude = true;
							}
							else if (!beta) stat = log(stat);
						} catch (const std::runtime_error& error) {
							num_not_converted++;
							exclude = true;
						}


						if(!alt_allele.empty() && SNP::ambiguous(ref_allele, alt_allele)){
							num_excluded++;
							exclude= true;
						}
						if(!exclude)
						{
							int category = -1;
							double pthres = 0.0;
							if (fastscore)
							{
								category = c_commander.get_category(pvalue);
								pthres = c_commander.get_threshold(category);
							}
							else
							{
								// calculate the threshold instead
								if (pvalue > bound_end && full)
								{
									category = std::ceil((bound_end + 0.1 - bound_start) / bound_inter);
									pthres = 1.0;
								}
								else
								{
									category = std::ceil((pvalue - bound_start) / bound_inter);
									category = (category < 0) ? 0 : category;
									pthres = category * bound_inter + bound_start;
								}
							}
							if(flipped) cur_snp.set_flipped();
							cur_snp.set_statistic(stat, 0.0, pvalue, category, pthres);
						}
					}
				}
				else if(dup_index.find(rs_id)!=dup_index.end())
				{
					num_duplicated++;
				}
				else
				{
					num_not_found++;
				}
			}
		}
	}
	snp_file.close();
/*
		for (size_t i_snp = 0; i_snp < m_snp_list.size(); ++i_snp)
		{
			m_snp_index[m_snp_list[i_snp].get_rs_id()] = i_snp;
			std::string cur_chr = m_snp_list[i_snp].get_chr();
			if (unique_chr.find(cur_chr) == unique_chr.end())
			{
				unique_chr.insert(cur_chr);
				m_chr_list.push_back(cur_chr);
			}
			std::string rs = m_snp_list[i_snp].get_rs_id();
			if(unique_rsid.find(rs)==unique_rsid.end())
			{
				unique_rsid.insert(rs);
			}
			else
			{
				std::string error_message = "WARNING: Duplicated SNP ID: " + rs;
				throw std::runtime_error(error_message);
			}
			if (index[+SNP_Index::CHR] >= 0 && index[+SNP_Index::BP] >= 0) // if we have chr and bp information
			{
				m_snp_list[i_snp].set_flag( region.check(cur_chr, m_snp_list[i_snp].get_loc()));
			}
		}
		*/
	if (num_excluded != 0) fprintf(stderr, "Number of SNPs excluded: %zu\n", num_excluded);
	if (num_not_found != 0) fprintf(stderr, "Number of SNPs not found in target: %zu\n", num_not_found);
	if (num_duplicated != 0) fprintf(stderr, "Number of duplicated SNPs : %zu\n", num_duplicated);
	fprintf(stderr, "Final Number of SNPs from base  : %zu\n", dup_index.size());
}