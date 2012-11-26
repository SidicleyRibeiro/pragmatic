/*  Copyright (C) 2010 Imperial College London and others.
 *
 *  Please see the AUTHORS file in the main source directory for a
 *  full list of copyright holders.
 *
 *  Gerard Gorman
 *  Applied Modelling and Computation Group
 *  Department of Earth Science and Engineering
 *  Imperial College London
 *
 *  g.gorman@imperial.ac.uk
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  1. Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following
 *  disclaimer in the documentation and/or other materials provided
 *  with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *  CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 *  BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *  TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 *  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 */

#include <iostream>
#include <vector>

#include <omp.h>

#include "Mesh.h"
#include "Surface.h"
#include "VTKTools.h"
#include "MetricField.h"

#include "Refine.h"
#include "ticker.h"

#include <mpi.h>

int main(int argc, char **argv){
#ifdef HAVE_MPI
  MPI::Init(argc, argv);
#endif

  int rank = 0;
#ifdef HAVE_MPI
  if(MPI::Is_initialized()){
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  }
#endif
  
  bool verbose = false;
  if(argc>1){
    verbose = std::string(argv[1])=="-v";
  }

  Mesh<double, int> *mesh=VTKTools<double, int>::import_vtu("../data/box10x10.vtu");

  Surface2D<double, int> surface(*mesh);
  surface.find_surface(true);

  MetricField2D<double, int> metric_field(*mesh, surface);

  size_t NNodes = mesh->get_number_nodes();
  double eta=0.00001;

  std::vector<double> psi(NNodes);
  for(size_t i=0;i<NNodes;i++){
    double x = 2*mesh->get_coords(i)[0]-1;
    double y = 2*mesh->get_coords(i)[1]-1;

    psi[i] = 0.100000000000000*sin(50*x) + atan2(-0.100000000000000, (double)(2*x - sin(5*y)));
  }

  metric_field.add_field(&(psi[0]), eta, 1);
  metric_field.update_mesh();

  VTKTools<double, int>::export_vtu("../data/test_refine_2d-initial", mesh);

  Refine2D<double, int> adapt(*mesh, surface);

  double tic = get_wtime();
  for(int i=0;i<5;i++)
    adapt.refine(sqrt(2.0));
  double toc = get_wtime();


  if(verbose)
    mesh->verify();

  std::vector<int> active_vertex_map;
  mesh->defragment(&active_vertex_map);
  surface.defragment(&active_vertex_map);

  VTKTools<double, int>::export_vtu("../data/test_refine_2d", mesh);
  VTKTools<double, int>::export_vtu("../data/test_refine_2d_surface", &surface);
  
  double lrms = mesh->get_lrms();
  double qrms = mesh->get_qrms();
  if(verbose){
    int nelements = mesh->get_number_elements();      
    if(rank==0)
      std::cout<<"Refine loop time:     "<<toc-tic<<std::endl
               <<"Number elements:      "<<nelements<<std::endl
               <<"Edge length RMS:      "<<lrms<<std::endl
               <<"Quality RMS:          "<<qrms<<std::endl;
  }

  if(rank==0){
    if((lrms<0.8)&&(qrms<0.3))
      std::cout<<"pass"<<std::endl;
    else
      std::cout<<"fail"<<std::endl;
  }

  delete mesh;

#ifdef HAVE_MPI
  MPI::Finalize();
#endif

  return 0;
}
