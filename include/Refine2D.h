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

#ifndef REFINE2D_H
#define REFINE2D_H

#include <algorithm>
#include <set>
#include <vector>

#include <string.h>
#include <inttypes.h>

#include "Edge.h"
#include "ElementProperty.h"
#include "Mesh.h"

/*! \brief Performs 2D mesh refinement.
 *
 */
template<typename real_t> class Refine2D{
 public:
  /// Default constructor.
  Refine2D(Mesh<real_t> &mesh){
    _mesh = &mesh;

    size_t NElements = _mesh->get_number_elements();

    // Set the orientation of elements.
    property = NULL;
    for(size_t i=0;i<NElements;i++){
      const int *n=_mesh->get_element(i);
      if(n[0]<0)
        continue;

      property = new ElementProperty<real_t>(_mesh->get_coords(n[0]),
                                             _mesh->get_coords(n[1]),
                                             _mesh->get_coords(n[2]));

      break;
    }

    MPI_Comm comm = _mesh->get_mpi_comm();

    nprocs = pragmatic_nprocesses(comm);
    rank = pragmatic_process_id(comm);

    nthreads = pragmatic_nthreads();

    newVertices.resize(nthreads);
    newElements.resize(nthreads);
    newBoundaries.resize(nthreads);
    newCoords.resize(nthreads);
    newMetric.resize(nthreads);

    threadIdx.resize(nthreads);
    splitCnt.resize(nthreads);
  }

  /// Default destructor.
  ~Refine2D(){
    delete property;
  }

  /*! Perform one level of refinement See Figure 25; X Li et al, Comp
   * Methods Appl Mech Engrg 194 (2005) 4915-4950. The actual
   * templates used for 3D refinement follows Rupak Biswas, Roger
   * C. Strawn, "A new procedure for dynamic adaption of
   * three-dimensional unstructured grids", Applied Numerical
   * Mathematics, Volume 13, Issue 6, February 1994, Pages 437-452.
   */
  void refine(real_t L_max){
    size_t origNElements = _mesh->get_number_elements();
    size_t origNNodes = _mesh->get_number_nodes();
    
    allNewVertices = new DirectedEdge<index_t>[100*origNElements];

#pragma omp parallel
    {
#pragma omp single nowait
      { 
        new_vertices_per_element.resize(3*origNElements);
        std::fill(new_vertices_per_element.begin(), new_vertices_per_element.end(), -1);
      }
      
      int tid = pragmatic_thread_id();
      splitCnt[tid] = 0;

      /*
       * Average vertex degree is ~6, so there
       * are approx. (6/2)*NNodes edges in the mesh.
       */
      size_t reserve_size = 3*origNNodes/nthreads;
      newVertices[tid].clear(); newVertices[tid].reserve(reserve_size);
      newCoords[tid].clear(); newCoords[tid].reserve(ndims*reserve_size);
      newMetric[tid].clear(); newMetric[tid].reserve(msize*reserve_size);

      /* Loop through all edges and select them for refinement if
         its length is greater than L_max in transformed space. */
#pragma omp for schedule(guided) nowait
      for(size_t i=0;i<origNNodes;++i){
        for(size_t it=0;it<_mesh->NNList[i].size();++it){
          index_t otherVertex = _mesh->NNList[i][it];
          assert(otherVertex>=0);
          
          /* Conditional statement ensures that the edge length is only calculated once.
           * By ordering the vertices according to their gnn, we ensure that all processes
           * calculate the same edge length when they fall on the halo.
           */
          if(_mesh->lnn2gnn[i] < _mesh->lnn2gnn[otherVertex]){
            double length = _mesh->calc_edge_length(i, otherVertex);
            if(length>L_max){
              ++splitCnt[tid];
              refine_edge(i, otherVertex, tid);
            }
          }
        }
      }
      
      threadIdx[tid] = pragmatic_omp_atomic_capture(&_mesh->NNodes, splitCnt[tid]);

#pragma omp barrier

#pragma omp single
      {      
	if(_mesh->_coords.size()<_mesh->NNodes*3){ 
	  _mesh->_coords.resize(_mesh->NNodes*ndims);
	  _mesh->metric.resize(_mesh->NNodes*msize);
	  _mesh->NNList.resize(_mesh->NNodes);
	  _mesh->NEList.resize(_mesh->NNodes);
	  _mesh->node_owner.resize(_mesh->NNodes);
	  _mesh->lnn2gnn.resize(_mesh->NNodes);
	}
      }

      // Append new coords and metric to the mesh.
      memcpy(&_mesh->_coords[ndims*threadIdx[tid]], &newCoords[tid][0], ndims*splitCnt[tid]*sizeof(real_t));
      memcpy(&_mesh->metric[msize*threadIdx[tid]], &newMetric[tid][0], msize*splitCnt[tid]*sizeof(double));
      
      // Fix IDs of new vertices
      assert(newVertices[tid].size()==splitCnt[tid]);
      for(size_t i=0;i<splitCnt[tid];i++){
        newVertices[tid][i].id = threadIdx[tid]+i;
      }

      // Accumulate all newVertices in a contiguous array
      memcpy(&allNewVertices[threadIdx[tid]-origNNodes], &newVertices[tid][0], newVertices[tid].size()*sizeof(DirectedEdge<index_t>));

      // Mark each element with its new vertices, update NNList
      // for all split edges.
#pragma omp barrier
#pragma omp for schedule(guided)
      for(size_t i=0; i<_mesh->NNodes-origNNodes; ++i){
        index_t vid = allNewVertices[i].id;
        index_t firstid = allNewVertices[i].edge.first;
        index_t secondid = allNewVertices[i].edge.second;

        // Find which elements share this edge and mark them with their new vertices.
        std::set<index_t> intersection;
        std::set_intersection( _mesh->NEList[firstid].begin(), _mesh->NEList[firstid].end(),
                               _mesh->NEList[secondid].begin(), _mesh->NEList[secondid].end(), std::inserter(intersection, intersection.begin()));
        
        for(typename std::set<index_t>::const_iterator element=intersection.begin(); element!=intersection.end(); ++element){
          index_t eid = *element;
          size_t edgeOffset = edgeNumber(eid, firstid, secondid);
          new_vertices_per_element[3*eid+edgeOffset] = vid;
        }
        
        /*
         * Update NNList for newly created vertices. This has to be done here, it cannot be
         * done during element refinement, because a split edge is shared between two elements
         * and we run the risk that these updates will happen twice, once for each element.
         */
        _mesh->NNList[vid].push_back(firstid);
        _mesh->NNList[vid].push_back(secondid);
        
        typename std::vector<index_t>::iterator it;
        it = std::find(_mesh->NNList[firstid].begin(), _mesh->NNList[firstid].end(), secondid);
        assert(it!=_mesh->NNList[firstid].end());
        *it = vid;
        
        it = std::find(_mesh->NNList[secondid].begin(), _mesh->NNList[secondid].end(), firstid);
        assert(it!=_mesh->NNList[secondid].end());
        *it = vid;
        
        /* If we have MPI, it makes no sense to update node_owner and lnn2gnn now because
         * these values are most probably wrong. However, when we have no MPI, updating the
         * values here saves us from an OMP parallel loop which kills performance (due to a
         * necessary thread synchronisation right after element refinement).
         */
        _mesh->node_owner[vid] = 0;
        _mesh->lnn2gnn[vid] = vid;
      }
      
      // Start element refinement.
#pragma omp for schedule(guided)
      for(size_t eid=0; eid<origNElements; ++eid){
        //If the element has been deleted, continue.
        const index_t *n = _mesh->get_element(eid);
        if(n[0] < 0)
          continue;
        
        for(size_t j=0; j<3; ++j)
          if(new_vertices_per_element[3*eid+j] != -1){
            refine_element(eid, tid);
            break;
          }
      }
      
      // Commit deferred operations.
#pragma omp for schedule(guided)
      for(int vtid=0; vtid<_mesh->defOp_scaling_factor*nthreads; ++vtid){
        _mesh->commit_deferred(vtid);
      }
      
      if(nprocs==1){
        // If we update lnn2gnn and node_owner here, OMP performance suffers.
      }else{
#ifdef HAVE_MPI
#pragma omp for schedule(static)
        for(size_t i=0; i<_mesh->NNodes-origNNodes; ++i){
          DirectedEdge<index_t> *vert = &allNewVertices[i];
          /*
           * Perhaps we should introduce a system of alternating min/max assignments,
           * i.e. one time the node is assigned to the min rank, one time to the max
           * rank and so on, so as to avoid having the min rank accumulate the majority
           * of newly created vertices and disturbing load balance among MPI processes.
           */
          int owner0 = _mesh->node_owner[vert->edge.first];
          int owner1 = _mesh->node_owner[vert->edge.second];
          int owner = std::min(owner0, owner1);
          _mesh->node_owner[vert->id] = owner;
        }
        
        // TODO: This single section can be parallelised
#pragma omp single
        {
          // Once the owner for all new nodes has been set, it's time to amend the halo.
          std::vector< std::set< DirectedEdge<index_t> > > recv_additional(nprocs), send_additional(nprocs);
          std::vector<index_t> invisible_vertices;
          
          for(size_t i=0; i<_mesh->NNodes-origNNodes; ++i){
            DirectedEdge<index_t> *vert = &allNewVertices[i];
            
            if(_mesh->node_owner[vert->id] != rank){
              // Vertex is owned by another MPI process, so prepare to update recv and recv_halo.
              // Only update them if the vertex is actually visible by *this* MPI process,
              // i.e. if at least one of its neighbours is owned by *this* process.
              bool visible = false;
              for(typename std::vector<index_t>::const_iterator neigh=_mesh->NNList[vert->id].begin(); neigh!=_mesh->NNList[vert->id].end(); ++neigh){
                if(_mesh->is_owned_node(*neigh)){
                  visible = true;
                  DirectedEdge<index_t> gnn_edge(_mesh->lnn2gnn[vert->edge.first], _mesh->lnn2gnn[vert->edge.second], vert->id);
                  recv_additional[_mesh->node_owner[vert->id]].insert(gnn_edge);
                  break;
                }
              }
              if(!visible)
                invisible_vertices.push_back(vert->id);
            }else{
              // Vertex is owned by *this* MPI process, so check whether it is visible by other MPI processes.
              // The latter is true only if both vertices of the original edge were halo vertices.
              if(_mesh->is_halo_node(vert->edge.first) && _mesh->is_halo_node(vert->edge.second)){
                // Find which processes see this vertex
                std::set<int> processes;
                for(typename std::vector<index_t>::const_iterator neigh=_mesh->NNList[vert->id].begin(); neigh!=_mesh->NNList[vert->id].end(); ++neigh)
                  processes.insert(_mesh->node_owner[*neigh]);
                
                processes.erase(rank);
                
                for(typename std::set<int>::const_iterator proc=processes.begin(); proc!=processes.end(); ++proc){
                  DirectedEdge<index_t> gnn_edge(_mesh->lnn2gnn[vert->edge.first], _mesh->lnn2gnn[vert->edge.second], vert->id);
                  send_additional[*proc].insert(gnn_edge);
                }
              }
            }
          }
          
          // Append vertices in recv_additional and send_additional to recv and send.
          // Mark how many vertices are added to each of these vectors.
          std::vector<size_t> recv_cnt(nprocs, 0), send_cnt(nprocs, 0);
          
          for(int i=0;i<nprocs;++i){
            recv_cnt[i] = recv_additional[i].size();
            for(typename std::set< DirectedEdge<index_t> >::const_iterator it=recv_additional[i].begin();it!=recv_additional[i].end();++it){
              _mesh->recv[i].push_back(it->id);
              _mesh->recv_halo.insert(it->id);
            }

            send_cnt[i] = send_additional[i].size();
            for(typename std::set< DirectedEdge<index_t> >::const_iterator it=send_additional[i].begin();it!=send_additional[i].end();++it){
              _mesh->send[i].push_back(it->id);
              _mesh->send_halo.insert(it->id);
            }
          }

          // Update global numbering
          for(size_t i=origNNodes; i<_mesh->NNodes; ++i)
            if(_mesh->node_owner[i] == rank)
              _mesh->lnn2gnn[i] = _mesh->gnn_offset+i;
          
          _mesh->update_gappy_global_numbering(recv_cnt, send_cnt);
        
          // Now that the global numbering has been updated, update send_map and recv_map.
          for(int i=0;i<nprocs;++i){
            for(typename std::set< DirectedEdge<index_t> >::const_iterator it=recv_additional[i].begin();it!=recv_additional[i].end();++it)
              _mesh->recv_map[i][_mesh->lnn2gnn[it->id]] = it->id;

            for(typename std::set< DirectedEdge<index_t> >::const_iterator it=send_additional[i].begin();it!=send_additional[i].end();++it)
              _mesh->send_map[i][_mesh->lnn2gnn[it->id]] = it->id;
          }
          
          _mesh->clear_invisible(invisible_vertices);
          _mesh->trim_halo();
        }
#endif
      }

#if !defined NDEBUG && !defined __FUJITSU
#pragma omp barrier
      // Fix orientations of new elements.
      size_t NElements = _mesh->get_number_elements();

#pragma omp for
      for(size_t i=0;i<NElements;i++){
        index_t n0 = _mesh->_ENList[i*nloc];
        if(n0<0)
          continue;

        index_t n1 = _mesh->_ENList[i*nloc + 1];
        index_t n2 = _mesh->_ENList[i*nloc + 2];

        const real_t *x0 = &_mesh->_coords[n0*ndims];
        const real_t *x1 = &_mesh->_coords[n1*ndims];
        const real_t *x2 = &_mesh->_coords[n2*ndims];

        real_t av = property->area(x0, x1, x2);

        if(av<=0){
#pragma omp critical
          std::cerr<<"ERROR: inverted element in refinement"<<std::endl
             <<"element = "<<n0<<", "<<n1<<", "<<n2<<std::endl;
          exit(-1);
        }
      }
#endif
    
#pragma omp single
      delete[] allNewVertices;
    }
  }

 private:

  void refine_edge(index_t n0, index_t n1, size_t tid){
    if(_mesh->lnn2gnn[n0] > _mesh->lnn2gnn[n1]){
      // Needs to be swapped because we want the lesser gnn first.
      index_t tmp_n0=n0;
      n0=n1;
      n1=tmp_n0;
    }
    newVertices[tid].push_back(DirectedEdge<index_t>(n0, n1));

    // Calculate the position of the new point. From equation 16 in
    // Li et al, Comp Methods Appl Mech Engrg 194 (2005) 4915-4950.
    real_t x, m;
    const real_t *x0 = _mesh->get_coords(n0);
    const double *m0 = _mesh->get_metric(n0);

    const real_t *x1 = _mesh->get_coords(n1);
    const double *m1 = _mesh->get_metric(n1);

    real_t weight = 1.0/(1.0 + sqrt(property->length(x0, x1, m0)/
                                    property->length(x0, x1, m1)));

    // Calculate position of new vertex and append it to OMP thread's temp storage
    for(size_t i=0;i<ndims;i++){
      x = x0[i]+weight*(x1[i] - x0[i]);
      newCoords[tid].push_back(x);
    }

    // Interpolate new metric and append it to OMP thread's temp storage
    for(size_t i=0;i<msize;i++){
      m = m0[i]+weight*(m1[i] - m0[i]);
      newMetric[tid].push_back(m);
      if(pragmatic_isnan(m))
        std::cerr<<"ERROR: metric health is bad in "<<__FILE__<<std::endl
                 <<"m0[i] = "<<m0[i]<<std::endl
                 <<"m1[i] = "<<m1[i]<<std::endl
                 <<"property->length(x0, x1, m0) = "<<property->length(x0, x1, m0)<<std::endl
                 <<"property->length(x0, x1, m1) = "<<property->length(x0, x1, m1)<<std::endl
                 <<"weight = "<<weight<<std::endl;
    }
  }

  int refine_element(index_t eid, size_t tid){
    const int *n=_mesh->get_element(eid);
    const int *boundary=&(_mesh->boundary[eid*3]);

    // Note the order of the edges - the i'th edge is opposite the i'th node in the element.
    index_t newVertex[3] = {-1, -1, -1};
    newVertex[0] = new_vertices_per_element[3*eid];
    newVertex[1] = new_vertices_per_element[3*eid+1];
    newVertex[2] = new_vertices_per_element[3*eid+2];

    size_t refine_cnt = 0;
    for(size_t i=0; i<3; ++i)
      if(newVertex[i]!=-1)
        ++refine_cnt;

    if(refine_cnt==1){
      // Single edge split.
      int rotated_ele[3];
      int rotated_boundary[3];
      index_t vertexID=-1;
      for(int j=0;j<3;j++)
        if(newVertex[j] >= 0){
          vertexID = newVertex[j];

          rotated_ele[0] = n[j];
          rotated_ele[1] = n[(j+1)%3];
          rotated_ele[2] = n[(j+2)%3];

          rotated_boundary[0] = boundary[j];
          rotated_boundary[1] = boundary[(j+1)%3];
          rotated_boundary[2] = boundary[(j+2)%3];

          break;
        }
      assert(vertexID!=-1);

      const index_t ele0[] = {rotated_ele[0], rotated_ele[1], vertexID};
      const index_t ele1[] = {rotated_ele[0], vertexID, rotated_ele[2]};

      const index_t ele0_boundary[] = {rotated_boundary[0], 0, rotated_boundary[2]};
      const index_t ele1_boundary[] = {rotated_boundary[0], rotated_boundary[1], 0};

      index_t ele1ID;
      ele1ID = pragmatic_omp_atomic_capture(&_mesh->NElements, 1);

      // Add rotated_ele[0] to vertexID's NNList
      _mesh->deferred_addNN(vertexID, rotated_ele[0], tid);
      // Add vertexID to rotated_ele[0]'s NNList
      _mesh->deferred_addNN(rotated_ele[0], vertexID, tid);

      // ele1ID is a new ID which isn't correct yet, it has to be
      // updated once each thread has calculated how many new elements
      // it created, so put ele1ID into addNE_fix instead of addNE.
      // Put ele1 in rotated_ele[0]'s NEList
      _mesh->deferred_addNE(rotated_ele[0], ele1ID, tid);

      // Put eid and ele1 in vertexID's NEList
      _mesh->deferred_addNE(vertexID, eid, tid);
      _mesh->deferred_addNE(vertexID, ele1ID, tid);

      // Replace eid with ele1 in rotated_ele[2]'s NEList
      _mesh->deferred_remNE(rotated_ele[2], eid, tid);
      _mesh->deferred_addNE(rotated_ele[2], ele1ID, tid);

      assert(ele0[0]>=0 && ele0[1]>=0 && ele0[2]>=0);
      assert(ele1[0]>=0 && ele1[1]>=0 && ele1[2]>=0);

      set_element(eid, ele0, ele0_boundary);
      set_element(ele1ID, ele1, ele1_boundary);

      return 1;
    }else if(refine_cnt==2){
      int rotated_ele[3];
      int rotated_boundary[3];
      index_t vertexID[2];
      for(int j=0;j<3;j++){
        if(newVertex[j] < 0){
          vertexID[0] = newVertex[(j+1)%3];
          vertexID[1] = newVertex[(j+2)%3];

          rotated_ele[0] = n[j];
          rotated_ele[1] = n[(j+1)%3];
          rotated_ele[2] = n[(j+2)%3];

	  rotated_boundary[0] = boundary[j];
          rotated_boundary[1] = boundary[(j+1)%3];
          rotated_boundary[2] = boundary[(j+2)%3];
	  
          break;
        }
      }

      real_t ldiag0 = _mesh->calc_edge_length(rotated_ele[1], vertexID[0]);
      real_t ldiag1 = _mesh->calc_edge_length(rotated_ele[2], vertexID[1]);

      const int offset = ldiag0 < ldiag1 ? 0 : 1;

      const index_t ele0[] = {rotated_ele[0], vertexID[1], vertexID[0]};
      const index_t ele1[] = {vertexID[offset], rotated_ele[1], rotated_ele[2]};
      const index_t ele2[] = {vertexID[0], vertexID[1], rotated_ele[offset+1]};

      const index_t ele0_boundary[] = {0, rotated_boundary[1], rotated_boundary[2]};
      const index_t ele1_boundary[] = {rotated_boundary[0], (offset==0)?rotated_boundary[1]:0, (offset==0)?0:rotated_boundary[2]};
      const index_t ele2_boundary[] = {(offset==0)?rotated_boundary[2]:0, (offset==0)?0:rotated_boundary[1], 0};

      index_t ele0ID, ele2ID;
      ele0ID = pragmatic_omp_atomic_capture(&_mesh->NElements, 2);
      ele2ID = ele0ID+1;

      // NNList: Connect vertexID[0] and vertexID[1] with each other
      _mesh->deferred_addNN(vertexID[0], vertexID[1], tid);
      _mesh->deferred_addNN(vertexID[1], vertexID[0], tid);

      // vertexID[offset] and rotated_ele[offset+1] are the vertices on the diagonal
      _mesh->deferred_addNN(vertexID[offset], rotated_ele[offset+1], tid);
      _mesh->deferred_addNN(rotated_ele[offset+1], vertexID[offset], tid);

      // rotated_ele[offset+1] is the old vertex which is on the diagonal
      // Add ele2 in rotated_ele[offset+1]'s NEList
      _mesh->deferred_addNE(rotated_ele[offset+1], ele2ID, tid);

      // Replace eid with ele0 in NEList[rotated_ele[0]]
      _mesh->deferred_remNE(rotated_ele[0], eid, tid);
      _mesh->deferred_addNE(rotated_ele[0], ele0ID, tid);

      // Put ele0, ele1 and ele2 in vertexID[offset]'s NEList
      _mesh->deferred_addNE(vertexID[offset], eid, tid);
      _mesh->deferred_addNE(vertexID[offset], ele0ID, tid);
      _mesh->deferred_addNE(vertexID[offset], ele2ID, tid);

      // vertexID[(offset+1)%2] is the new vertex which is not on the diagonal
      // Put ele0 and ele2 in vertexID[(offset+1)%2]'s NEList
      _mesh->deferred_addNE(vertexID[(offset+1)%2], ele0ID, tid);
      _mesh->deferred_addNE(vertexID[(offset+1)%2], ele2ID, tid);

      assert(ele0[0]>=0 && ele0[1]>=0 && ele0[2]>=0);
      assert(ele1[0]>=0 && ele1[1]>=0 && ele1[2]>=0);
      assert(ele2[0]>=0 && ele2[1]>=0 && ele2[2]>=0);

      set_element(eid, ele1, ele1_boundary);
      set_element(ele0ID, ele0, ele0_boundary);
      set_element(ele2ID, ele2, ele2_boundary);

      return 2;
    }else{ // refine_cnt==3
      const index_t ele0[] = {n[0], newVertex[2], newVertex[1]};
      const index_t ele1[] = {n[1], newVertex[0], newVertex[2]};
      const index_t ele2[] = {n[2], newVertex[1], newVertex[0]};
      const index_t ele3[] = {newVertex[0], newVertex[1], newVertex[2]};

      const int ele0_boundary[] = {0, boundary[1], boundary[2]};
      const int ele1_boundary[] = {0, boundary[2], boundary[0]};
      const int ele2_boundary[] = {0, boundary[0], boundary[1]};
      const int ele3_boundary[] = {0, 0, 0};

      index_t ele1ID, ele2ID, ele3ID;
      ele1ID = pragmatic_omp_atomic_capture(&_mesh->NElements, 3);
      ele2ID = ele1ID+1;
      ele3ID = ele1ID+2;


      // Update NNList
      _mesh->deferred_addNN(newVertex[0], newVertex[1], tid);
      _mesh->deferred_addNN(newVertex[0], newVertex[2], tid);
      _mesh->deferred_addNN(newVertex[1], newVertex[0], tid);
      _mesh->deferred_addNN(newVertex[1], newVertex[2], tid);
      _mesh->deferred_addNN(newVertex[2], newVertex[0], tid);
      _mesh->deferred_addNN(newVertex[2], newVertex[1], tid);

      // Update NEList
      _mesh->deferred_remNE(n[1], eid, tid);
      _mesh->deferred_addNE(n[1], ele1ID, tid);
      _mesh->deferred_remNE(n[2], eid, tid);
      _mesh->deferred_addNE(n[2], ele2ID, tid);

      _mesh->deferred_addNE(newVertex[0], ele1ID, tid);
      _mesh->deferred_addNE(newVertex[0], ele2ID, tid);
      _mesh->deferred_addNE(newVertex[0], ele3ID, tid);

      _mesh->deferred_addNE(newVertex[1], eid, tid);
      _mesh->deferred_addNE(newVertex[1], ele2ID, tid);
      _mesh->deferred_addNE(newVertex[1], ele3ID, tid);

      _mesh->deferred_addNE(newVertex[2], eid, tid);
      _mesh->deferred_addNE(newVertex[2], ele1ID, tid);
      _mesh->deferred_addNE(newVertex[2], ele3ID, tid);

      assert(ele0[0]>=0 && ele0[1]>=0 && ele0[2]>=0);
      assert(ele1[0]>=0 && ele1[1]>=0 && ele1[2]>=0);
      assert(ele2[0]>=0 && ele2[1]>=0 && ele2[2]>=0);
      assert(ele3[0]>=0 && ele3[1]>=0 && ele3[2]>=0);

      set_element(eid, ele0, ele0_boundary);
      set_element(ele1ID, ele1, ele1_boundary);
      set_element(ele2ID, ele2, ele2_boundary);
      set_element(ele3ID, ele3, ele3_boundary);

      return 3;
    }
  }
  
  inline void set_element(const index_t eid, const index_t *element, const int *boundary){
    for(size_t i=0; i<nloc; ++i){
      _mesh->_ENList[eid*nloc+i]=element[i];
      _mesh->boundary[eid*nloc+i]=boundary[i];
    }
  }

  inline size_t edgeNumber(index_t eid, index_t v1, index_t v2) const{
    /*
     * Edge 0 is the edge (n[1],n[2]).
     * Edge 1 is the edge (n[0],n[2]).
     * Edge 2 is the edge (n[0],n[1]).
     */
    const int *n=_mesh->get_element(eid);
    if(n[1]==v1 || n[1]==v2){
      if(n[2]==v1 || n[2]==v2)
        return 0;
      else
        return 2;
    }
    else
      return 1;
  }

  std::vector< std::vector< DirectedEdge<index_t> > > newVertices;
  std::vector< std::vector<real_t> > newCoords;
  std::vector< std::vector<double> > newMetric;
  std::vector< std::vector<index_t> > newElements;
  std::vector< std::vector<int> > newBoundaries;
  std::vector<index_t> new_vertices_per_element;

  std::vector<size_t> threadIdx, splitCnt;
  DirectedEdge<index_t> *allNewVertices;

  Mesh<real_t> *_mesh;
  ElementProperty<real_t> *property;

  static const size_t ndims=2, nloc=3, msize=3;
  int nprocs, rank, nthreads;
};

#endif