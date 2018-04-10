// @file:     sim_anneal.cc
// @author:   Samuel
// @created:  2017.08.23
// @editted:  2017.08.23 - Samuel
// @license:  GNU LGPL v3
//
// @desc:     Simulated annealing physics engine

#include "sim_anneal.h"
#include <ctime>

using namespace phys;

SimAnneal::SimAnneal(const std::string& i_path, const std::string& o_path)
{
  phys_con = new PhysicsConnector(std::string("SimAnneal"), i_path, o_path);
  rng.seed(std::time(NULL));
  initExpectedParams();
}


void SimAnneal::initExpectedParams()
{
  std::cout << "SimAnneal instantiated." << std::endl;
  phys_con->setRequiredSimParam("preanneal_cycles");
  phys_con->setRequiredSimParam("anneal_cycles");
  phys_con->setRequiredSimParam("global_v0");
  phys_con->setRequiredSimParam("debye_length");
  phys_con->setRequiredSimParam("result_queue_size");
  phys_con->setExpectDB(true);
  phys_con->readProblem();
  for (auto& iter : phys_con->getRequiredSimParam()) {
    if(!phys_con->parameterExists(iter)){
      std::cout << "Parameter " << iter << " not found." << std::endl;
    }
  }
}


void SimAnneal::exportData()
{
  // create the vector of strings for the db locations
  std::vector<std::vector<std::string>> dbl_data(db_locs.size());
  for (unsigned int i = 0; i < db_locs.size(); i++) { //need the index
    dbl_data[i].resize(2);
    dbl_data[i][0] = std::to_string(db_locs[i].first);
    dbl_data[i][1] = std::to_string(db_locs[i].second);
  }
  phys_con->setExportDBLoc(true);
  phys_con->setDBLocData(dbl_data);

  std::vector<std::vector<std::string>> db_dist_data(db_charges.size());
  //unsigned int i = 0;
  for (unsigned int i = 0; i < db_charges.size(); i++) {
  //for (auto db_charge : db_charges) {
    db_dist_data[i].resize(2);
    std::string dbc_link;
    for(auto chg : db_charges[i]){
      dbc_link.append(std::to_string(chg));
    }
    db_dist_data[i][0] = dbc_link;
    db_dist_data[i][1] = std::to_string(config_energies[i]);
    // std::cout << db_dist_data[i][0] << std::endl;
  }

  phys_con->setExportDBElecConfig(true);
  phys_con->setDBElecData(db_dist_data);

  phys_con->writeResultsXml();
}


bool SimAnneal::runSim()
{
  // grab all physical locations (in original distance unit)
  // TODO move to phys_engine
  std::cout << "Grab all physical locations..." << std::endl;
  n_dbs = 0;
  phys_con->initCollections();
  for(auto db : *(phys_con->db_col)) {
    if(db->elec != 1){
      db_locs.push_back(std::make_pair(db->x, db->y));
      n_dbs++;
      std::cout << "DB loc: x=" << db_locs.back().first
          << ", y=" << db_locs.back().second << std::endl;
    }
    else
      fixed_charges.push_back(std::make_tuple(db->x, db->y, db->elec));
  }
  std::cout << "Free dbs, n_dbs=" << n_dbs << std::endl << std::endl;

  // exit if no dbs
  if(n_dbs == 0) {
    std::cout << "No dbs found, nothing to simulate. Exiting." << std::endl;
    return false;
  }

  // initialize variables & perform pre-calculation
  initVars();
  precalc();


  // SIM ANNEAL
  simAnneal();

  return true;
}


// PRIVATE

void SimAnneal::initVars()
{
  std::cout << "Initializing variables..." << std::endl;
  if(n_dbs <= 0){
    std::cout << "There are no dbs in the problem!" << std::endl;
    return;
  }
  t_preanneal = phys_con->parameterExists("preanneal_cycles") ?
                  std::stoi(phys_con->getParameter("preanneal_cycles")) : 1000;
  t_max = phys_con->parameterExists("anneal_cycles") ?
                  std::stoi(phys_con->getParameter("anneal_cycles")) : 10000;
  v_0 = phys_con->parameterExists("global_v0") ?
                  std::stof(phys_con->getParameter("global_v0")) : 1; // TODO this should be fixed
  debye_length = phys_con->parameterExists("debye_length") ?
                  std::stof(phys_con->getParameter("debye_length")) : 5E-9; // ~10s of dimer rows

  result_queue_size = phys_con->parameterExists("result_queue_size") ?
                  std::stoi(phys_con->getParameter("result_queue_size")) : 1000;
  result_queue_size = t_max < result_queue_size ? t_max : result_queue_size;

  Kc = 1/(4 * constants::PI * constants::EPS_SURFACE * constants::EPS0);
  kT = 2.568E-2; kT_step = 0.999999;    // kT = Boltzmann constant (eV/K) * 298 K, NOTE kT_step arbitrary
  v_freeze = 0, v_freeze_step = 0.001;  // NOTE v_freeze_step arbitrary

  // resize vectors
  v_eff.resize(n_dbs);
  v_ext.resize(n_dbs);
  v_drive.resize(n_dbs);
  v_ij.resize(n_dbs);
  db_r.resize(n_dbs);

  db_charges.resize(result_queue_size);
  db_charges.push_back(std::vector<int>(n_dbs));
  curr_charges = db_charges.back();

  config_energies.resize(result_queue_size);
  config_energies.push_back(0);

  std::cout << "Variable initialization complete" << std::endl << std::endl;
}

void SimAnneal::precalc()
{
  std::cout << "Performing pre-calculation..." << std::endl;
  if(n_dbs <= 0){
    std::cout << "There are no dbs in the problem!" << std::endl;
    return;
  }

  for(int i=0; i<n_dbs; i++) {
    db_r[i].resize(n_dbs);
    v_ij[i].resize(n_dbs);
    for(int j=0; j<n_dbs; j++) {
      if (i>j) {
        db_r[i][j] = db_r[j][i];
        v_ij[i][j] = v_ij[j][i];
      }
      else if (i==j) {
        db_r[i][j] = 0;
        v_ij[i][j] = div_0;
      }
      else {
        db_r[i][j] = distance(db_locs[i].first, db_locs[i].second, db_locs[j].first, db_locs[j].second)*db_distance_scale;
        v_ij[i][j] = interElecPotential(db_r[i][j]);
        std::cout << "db_r[" << i << "][" << j << "]=" << db_r[i][j] << ", v_ij[" << i << "][" << j << "]=" << v_ij[i][j] << std::endl;
        // TODO: db_r in bohr length
      }
    }

    // effect from fixed charges
    v_drive[i] = 0;
    for(std::tuple<float,float,float> fc : fixed_charges) {
      float r = distance(std::get<0>(fc), std::get<1>(fc), db_locs[i].first, db_locs[i].second)*db_distance_scale;
      v_drive[i] += interElecPotential(r);
    }
    std::cout << "v_drive["<<i<<"]="<<v_drive[i]<<std::endl;
    // TODO add electrode effect to v_ext

    v_eff[i] = 0;
    v_ext[i] = 0;
    //db_charges[i] = 0;
  }
  std::cout << "Pre-calculation complete" << std::endl << std::endl;
}


void SimAnneal::simAnneal()
{
  std::cout << "Performing simulated annealing..." << std::endl;

  // Vars
  float E_begin, E_end;
  int i=0,j=0;
  int from_ind, to_ind; // hopping from -> to (indices)
  int hop_count;
  float E_pre_hop, E_post_hop;

  // Run simulated annealing for predetermined time steps
  while(t < t_max) {
    E_begin = systemEnergy();


    // Population
    std::cout << "Population update, v_freeze=" << v_freeze << ", kT=" << kT << std::endl;
    for(i=0; i<n_dbs; i++) {
      v_eff[i] = v_0 + v_ext[i] - v_drive[i];
      for(j=0; j<n_dbs; j++)
        if(i!=j)
          v_eff[i] -= v_ij[i][j] * curr_charges[j];
    }
    for (i=0; i<n_dbs; i++)
      if (acceptPop(i))
        curr_charges[i] = !curr_charges[i];

    printCharges();


    // Hopping
    std::cout << "Hopping" << std::endl;
    hop_count = 0;
    int unocc_count = chargedDBCount(1);
    while(hop_count < unocc_count*5) {
      // TODO instead of finding system energy twice, could just find the potential difference of the db between pre- and post-hop
      E_pre_hop = systemEnergy(); // original energy
      from_ind = getRandDBInd(1);
      to_ind = getRandDBInd(0);

      if(from_ind == -1 || to_ind == -1)
        break; // hopping not possible

      // perform the hop
      dbHop(from_ind, to_ind);
      E_post_hop = systemEnergy(); // new energy

      // accept hop given energy change? reverse hop if energy delta is unaccpted
      if(!acceptHop(E_post_hop-E_pre_hop))
        dbHop(to_ind, from_ind);
        //curr_charges[from_ind] = 1, curr_charges[to_ind] = 0;
      else{
        std::cout << "Hop performed: ";
        printCharges();
        std::cout << "Energy diff=" << E_post_hop-E_pre_hop << std::endl;
      }
      hop_count++;
    }
    std::cout << "Charge post-hop=";
    printCharges();

    E_end = systemEnergy();

    // push back the new arrangement
    db_charges.push_back(curr_charges);
    config_energies.push_back(E_end);

    // perform time-step if not pre-annealing
    if(t_preanneal > 0)
      t_preanneal--;
    else
      timeStep();

    // print statistics
    std::cout << "Cycle: " << ((t_preanneal > 0) ? -t_preanneal : t);
    std::cout << ", ending energy: " << E_end;
    std::cout << ", delta: " << E_end-E_begin << std::endl << std::endl;
  }
}


void SimAnneal::dbHop(int from_ind, int to_ind)
{
  curr_charges[from_ind] = 0;
  curr_charges[to_ind] = 1;
}


void SimAnneal::timeStep()
{
  t++;
  // TODO kT and v_freeze schedules still need refinement
  kT *= kT_step;
  v_freeze += v_freeze_step;
}


void SimAnneal::printCharges()
{
  for(int i=0; i<n_dbs; i++)
    std::cout << curr_charges[i];
  //for(int i : *curr_charges)
  //  std::cout << i;
  std::cout << std::endl;
}





// ACCEPTANCE FUNCTIONS


bool SimAnneal::acceptPop(int db_ind)
{
  int curr_charge = curr_charges[db_ind];
  float v = curr_charge ? v_eff[db_ind] + v_freeze : - v_eff[db_ind] + v_freeze; // 1->0 : 0->1
  float prob;

  prob = 1. / ( 1 + exp( v/kT ) );

  //std::cout << "v_eff=" << v_eff[db_ind] << ", P(" << curr_charge << "->" << !curr_charge << ")=" << prob << std::endl;

  return evalProb(prob);
}


// acceptance function for hopping
bool SimAnneal::acceptHop(float v_diff)
{
  if (v_diff < 0)
    return true;

  // some acceptance function, acceptance probability falls off exponentially
  float prob = exp(-v_diff/kT);
  return evalProb(prob);
}


// takes a probability and generates true/false accordingly
bool SimAnneal::evalProb(float prob)
{
  boost::random::uniform_real_distribution<float> dis(0,1);
  boost::variate_generator<boost::random::mt19937&, boost::random::uniform_real_distribution<float>> rnd_gen(rng, dis);

  float generated_num = rnd_gen();
  //std::cout << "Probability: True if lower than " << prob << ", evaluation " << generated_num << std::endl;

  return prob >= generated_num;
}





// ACCESSORS


int SimAnneal::chargedDBCount(int charge)
{
  int i=0;
  for(int db_charge : curr_charges)
    if(db_charge == charge)
      i++;
  return i;
}


int SimAnneal::getRandDBInd(int charge)
{
  std::vector<int> dbs;

  // store the indices of dbs that have the desired occupation
  for (unsigned int i=0; i<curr_charges.size(); i++)
    if (curr_charges[i] == charge)
      dbs.push_back(i);

  if (dbs.empty())
    return -1; // no potential candidates

  // pick one from them
  boost::random::uniform_int_distribution<int> dis(0,dbs.size()-1);
  boost::variate_generator<boost::random::mt19937&, boost::random::uniform_int_distribution<int>> rnd_gen(rng, dis);
  // TODO move these to init and make them class var, not reinitialize it every time

  return dbs[rnd_gen()];
}





// PHYS CALCU


float SimAnneal::systemEnergy()
{
  assert(n_dbs > 0);
  float v = 0;
  for(int i=0; i<n_dbs; i++) {
    v += v_0 + curr_charges[i] * (v_ext[i] + v_drive[i]); // TODO combine v_ext and v_drive since they're not changing anyway (but somewhere above there's v_ext - v_drive, investigate)
    for(int j=i+1; j<n_dbs; j++)
      v += curr_charges[i] * curr_charges[j] * v_ij[i][j];
  }
  //return v * har_to_ev; // revert back to this when going back to hartree calculations
  return v;
}


float SimAnneal::distance(float x1, float y1, float x2, float y2)
{
  return sqrt(pow(x1-x2, 2.0) + pow(y1-y2, 2.0));
}


float SimAnneal::interElecPotential(float r)
{
  //return exp(-r/debye_length) / r;
  return constants::Q0 * Kc * exp(-r/debye_length) / r;
}
