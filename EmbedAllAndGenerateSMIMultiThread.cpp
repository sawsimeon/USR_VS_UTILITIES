/**
 * @file EmbedPDBQTMols.cpp
 * @author Louai KASSA BAGHDOUCHE
 * @brief A C++ code to generate conformers from multiple pdbqt files, by fetching the SMILES and ID from PDBQT and generate SMI files
 * @date 2022-03-29
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <future>
#include <GraphMol/FileParsers/MolSupplier.h>
#include <GraphMol/DistGeomHelpers/Embedder.h>
#include <GraphMol/FileParsers/MolWriters.h>
#include <GraphMol/FragCatalog/FragFPGenerator.h>
#include <GraphMol/SmilesParse/SmilesParse.h>
#include <GraphMol/Descriptors/MolDescriptors.h>
#include <GraphMol/Substruct/SubstructMatch.h>
#include <boost/filesystem/fstream.hpp>
#include <mutex>
#include <thread>
#include <boost/filesystem/operations.hpp>

using namespace std;
using namespace std::chrono;
using namespace RDKit;
using namespace RDGeom;
using namespace RDKit::Descriptors;
using namespace RDKit::MolOps;
using namespace RDKit::DGeomHelpers;
using namespace boost::filesystem;
using boost_ifstream = boost::filesystem::ifstream;
using boost_ofstream = boost::filesystem::ofstream;

class MoleculesProcess
{
public:
  explicit MoleculesProcess(const string& conformers_file, const string& smi_file)
  {
    m_Conformers_path = conformers_file;
    m_Smi_path = smi_file;
    int p = smi_file.find('.');
    m_Only_smiles = smi_file.substr(0, p) + "_only_smiles.txt";
    m_Only_id = smi_file.substr(0, p) + "_only_id.txt";
    p = conformers_file.find('.');
    m_Rfprop_file = conformers_file.substr(0, p) + "_4properties.f32";
    m_Riprop_file = conformers_file.substr(0, p) + "_5properties.i16";
    m_Usrcat_file = conformers_file.substr(0, p) + "_usrcat.f64";

    // m_Conf_file.open()
  }
  ~MoleculesProcess() 
  {
  }

  template<typename T>
  static float dist2(const T& p0, const T& p1)
  {
    lock_guard<mutex> lock(m_Mu); // should i leave this here?
    const auto d0 = p0[0] - p1[0];
    const auto d1 = p0[1] - p1[1];
    const auto d2 = p0[2] - p1[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
  }

  template<typename T>
  static void write_binary(T& buf);

  static inline void stripWhiteSpaces(string& str)
  {
    str.erase(remove(str.begin(), str.end(), ' '), str.end());  
  }

  void process(const size_t start_chunk, const size_t end_chunk, vector<path>& pdbqts_vector)
  {
    lock_guard<mutex> lock(m_Mu);
    string line, compound, smiles, next_line;
    int pos;
    EmbedParameters params(srETKDGv3);
    params.randomSeed = 209;
    params.numThreads = 4; // this may cause a problem
    params.useRandomCoords = true;
    params.maxIterations = 3;
    m_Conf_file.open(m_Conformers_path, ios::app);
    SDWriter writer(&m_Conf_file);
    for (size_t counter = start_chunk; counter < end_chunk; ++counter)
    {
      pdbqt_ifs.open(pdbqts_vector[counter]);
      while (getline(pdbqt_ifs, line))
      {
        if (line.find("Compound:") != string::npos)
        {
          pos = line.find(':');
          compound = line.substr(pos + 1);
          MoleculesProcess::stripWhiteSpaces(compound);
        }
        if (line.find("SMILES:") != string::npos)
        {
          pos = line.find(':');
          smiles = line.substr(pos + 1);
          MoleculesProcess::stripWhiteSpaces(smiles);
          if (getline(pdbqt_ifs, next_line))
          {
            if (!next_line.find("REMARK") != string::npos)
            {
              // smiles is splitted, trying to fix it
              smiles = smiles + next_line;
            }
          }
          if (smiles.find('q') != string::npos || smiles.find('r') != string::npos || smiles.find('s') != string::npos)
          {
            // incorrect format of the smiles
            break;
          }
          else
          {
            try 
            {
              const unique_ptr<ROMol> smi_ptr(SmilesToMol(smiles));
              const unique_ptr<ROMol> mol_ptr(addHs(*smi_ptr));
              auto& mol = *mol_ptr;
              mol.setProp("_Name", compound);
              const auto confIds = EmbedMultipleConfs(mol, 4, params);
              if (confIds.empty())
                break;
              if (confIds.size() == 4)
              {
                m_Realfprop[0] = calcExactMW(mol);
                m_Realfprop[1] = calcClogP(mol);
                m_Realfprop[2] = calcTPSA(mol);
                m_Realfprop[3] = calcLabuteASA(mol);


                m_Realiprop[0] = mol.getNumHeavyAtoms();
                m_Realiprop[1] = calcNumHBD(mol);
                m_Realiprop[2] = calcNumHBA(mol);
                m_Realiprop[3] = calcNumRotatableBonds(mol);
                m_Realiprop[4] = calcNumRings(mol);

                m_rfprop.open(m_Rfprop_file, ios::binary | ios::app);
                m_riprop.open(m_Riprop_file, ios::binary | ios::app);
                const size_t num_bytes_realf = sizeof(m_Realfprop);
                m_rfprop.write(reinterpret_cast<char*>(m_Realfprop.data()), num_bytes_realf);
                const size_t num_bytes_reali = sizeof(m_Realiprop);
                m_riprop.write(reinterpret_cast<char*>(m_Realiprop.data()), num_bytes_reali);

                cout << confIds.size() << " Conformers of " << compound << " : " << smiles << " are successfully generated!" << endl;
                m_Id_file.open(m_Only_id, ios::app);
                m_Smiles_file.open(m_Only_smiles, ios::app);
                m_Smi_file.open(m_Smi_path, ios::app);
                m_usrcatf64.open(m_Usrcat_file, ios::binary | ios::app);
                
                m_Id_file << compound << '\n';
                m_Smiles_file << smiles << '\n';
                m_Smi_file << compound << '\t' << smiles << '\n';

                for (const auto confId : confIds)
                {
                  writer.write(mol, confId);
                }

                for (int i = 0; i < 4; i++)
                {
                  m_Features = usrcat_features(mol, i);
                  const size_t num_bytes = sizeof(m_Features);
                  m_usrcatf64.write(reinterpret_cast<char*>(m_Features.data()), num_bytes);
                }
                m_Id_file.close();
                m_Smiles_file.close();
                m_Smi_file.close();
                m_Conf_file.close();
                m_riprop.close();
                m_rfprop.close();
                m_usrcatf64.close();
              }
            }
            catch (const runtime_error& e)
            {
              break;
            }
          }
        }
      }
    }
  }

private:
  mutex m_Mu;
  path m_Conformers_path, m_Smi_path, m_Only_smiles, m_Only_id, m_Rfprop_file, m_Riprop_file, m_Usrcat_file;
  boost_ofstream m_Conf_file, m_Smi_file, m_Smiles_file, m_Id_file, m_rfprop, m_riprop, m_usrcatf64;
  boost_ifstream pdbqt_ifs;
  array<float, 4> m_Realfprop;
  array<int16_t, 5> m_Realiprop;
  array<float, 60> m_Features;
  array<float, 60> usrcat_features(ROMol& mol, int index)
  {
    lock_guard<mutex> lock(m_Mu);
    const size_t num_references = 4;
    const size_t num_subsets = 5;
    const array<string, 5> SubsetSMARTS
    {{
      "[!#1]", // heavy
      "[#6+0!$(*~[#7,#8,F]),SH0+0v2,s+0,S^3,Cl+0,Br+0,I+0]", // hydrophobic
      "[a]", // aromatic
      "[$([O,S;H1;v2]-[!$(*=[O,N,P,S])]),$([O,S;H0;v2]),$([O,S;-]),$([N&v3;H1,H2]-[!$(*=[O,N,P,S])]),$([N;v3;H0]),$([n,o,s;+0]),F]", // acceptor
      "[N!H0v3,N!H0+v4,OH+0,SH+0,nH+0]", // donor
    }};
    array<unique_ptr<ROMol>, 5> SubsetMols;
    array<vector<int>, 5> subsets;
    array<Point3D, 4> references;
    array<vector<float>, 4> dista;
    array<float, 60> features;
    // Wrap SMARTS strings to ROMol objects.
    for (size_t k = 0; k < num_subsets; ++k)
    {
      SubsetMols[k].reset(reinterpret_cast<ROMol*>(SmartsToMol(SubsetSMARTS[k])));
    }
    const auto num_points = mol.getNumHeavyAtoms();
    const auto& conformer = mol.getConformer(index);
    for (size_t k = 0; k < num_subsets; ++k)
    {
      vector<vector<pair<int, int>>> matchVect;
      SubstructMatch(mol, *SubsetMols[k], matchVect);
      const auto num_matches = matchVect.size();
      auto& subset = subsets[k];
      subset.resize(num_matches);
      for (size_t j = 0; j < num_matches; ++j)
      {
        subset[j] = matchVect[j].front().second;
      }
    }
    const auto& subset0 = subsets.front();
    // assert(subset0.size() == num_points);

    for (auto& ref : references)
    {
      ref.x = ref.y = ref.z = 0;
    }
    auto& ctd = references[0];
    auto& cst = references[1];
    auto& fct = references[2];
    auto& ftf = references[3];
    for (const auto sub : subset0)
    {
      const auto& a = conformer.getAtomPos(sub);
      ctd += a;
    }
    ctd /= num_points;
    float cst_dist = numeric_limits<float>::max();
    float fct_dist = numeric_limits<float>::lowest();
    float ftf_dist = numeric_limits<float>::lowest();
    for (const auto sub : subset0)
    {
      const auto& a = conformer.getAtomPos(sub);
      const auto this_dist = dist2(a, ctd);
      if (this_dist < cst_dist)
      {
        cst = a;
        cst_dist = this_dist;
      }
      if (this_dist > fct_dist)
      {
        fct = a;
        fct_dist = this_dist;
      }
    }
    for (const auto sub : subset0)
    {
      const auto& a = conformer.getAtomPos(sub);
      const auto this_dist = dist2(a, fct);
      if (this_dist > ftf_dist)
      {
        ftf = a;
        ftf_dist = this_dist;
      }
    }
    // Precalculate the distances of heavy atoms to the reference points, given that subsets[1 to 4] are subsets of subsets[0].
    for (size_t ref = 0; ref < num_references; ++ref)
    {
      const auto& reference = references[ref];
      auto& distp = dista[ref];
      distp.resize(num_points);
      for (size_t p = 0; p < num_points; ++p)
      {
        distp[subset0[p]] = sqrt(dist2(conformer.getAtomPos(subset0[p]), reference));
      }
    }
    // loop over pharmacophoric subsets and reference points.
    size_t qo = 0;
    for (const auto& subset : subsets)
    {
      const auto n = subset.size();
      for (size_t ref = 0; ref < num_references; ++ref)
      {
        // Load distances from precalculated ones
        const auto& distp = dista[ref];
        vector<float> dists(n);
        for (size_t a = 0; a < n; ++a)
        {
          dists[a] = distp[subset[a]];
        }
        // Compute moments
        array<float, 3> m{};
        if (n > 2)
        {
          const auto v = 1.0 / n;
          for (size_t j = 0; j < n; ++j)
          {
            const auto d = dists[j];
            m[0] += d;
          }
          m[0] *= v;
          for (size_t j = 0; j < n; ++j)
          {
            const auto d = dists[j] - m[0];
            m[1] += d * d;
          }
          m[1] = sqrt(m[1] * v);
          for (size_t j = 0; j < n; ++j)
          {
            const auto d = dists[j] - m[0];
            m[2] += d * d * d;
          }
          m[2] = cbrt(m[2] * v);
        }
        else if (n == 2)
        {
          m[0] = 0.5 * (dists[0] + dists[1]);
          m[1] = 0.5 * fabs(dists[0] - dists[1]);
        }
        else if (n == 1)
        {
          m[0] = dists[0];
        }
        for (const auto e : m)
        {
          features[qo++] = e;
        }
      }
    }
    return features;
  }
};


int main(int argc, char* argv[])
{
  if (argc != 5)
  {
    cerr << "Usage: ./EmbedAllAndGenerateSMIL [PDBQT FOLDER] [CONFORMERS SDF] [OUTPUT SMI] [NUM_PDBQT]" << endl;
    return 1;
  }

  // Obtain the files from the CLI argument
  const auto pdbqt_folder = argv[1];
  const auto conformers_file = argv[2];
  const auto smi_file = argv[3];
  const auto num_of_pdbqt = atoi(argv[4]);

  // Initalize constant
  const string pdbqt_extension = ".pdbqt";
  const int num_threads = 30;
  // Initialize vectors
  vector<path> pdbqts_vector;
  vector<future<void>> thread_pool;
  pdbqts_vector.reserve(num_of_pdbqt);
  thread_pool.reserve(num_threads); // a thread pool with 30 threads

  // object
  MoleculesProcess worker(conformers_file, smi_file);

  // Search for pdbqt files into the pdbqt folder
  for (const auto& entry : recursive_directory_iterator(pdbqt_folder))
  {
    if (entry.path().extension() == pdbqt_extension)
    {
      pdbqts_vector.push_back(entry.path());
    }
  }

  size_t start = 0;
  size_t num_files = pdbqts_vector.size();
  size_t chunk_size = (num_files - start) / num_threads;

  for (int i = 0; i < num_threads; i++)
  {
    size_t start_chunk = chunk_size * i;
    size_t end_chunk = chunk_size * i + chunk_size;
    thread_pool.emplace_back(async(launch::async, &MoleculesProcess::process, &worker, start_chunk, end_chunk, ref(pdbqts_vector)));
  }

  for (auto& thread : thread_pool)
    thread.get();
}
