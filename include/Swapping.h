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

#ifndef SWAPPING_H
#define SWAPPING_H

#include <algorithm>
#include <set>
#include <vector>
#include <limits>

#include "ElementProperty.h"
#include "Mesh.h"
#include "Colour.h"

/*! \brief Performs edge/face swapping.
 *
 */
template<typename real_t, typename index_t> class Swapping{
 public:
  /// Default constructor.
  Swapping(Mesh<real_t, index_t> &mesh, Surface<real_t, index_t> &surface){
    _mesh = &mesh;
    _surface = &surface;
    
    size_t NElements = _mesh->get_number_elements();
    ndims = _mesh->get_number_dimensions();
    nloc = (ndims==2)?3:4;

    // Set the orientation of elements.
    property = NULL;
    for(size_t i=0;i<NElements;i++){
      const int *n=_mesh->get_element(i);
      if(n[0]<0)
        continue;
      
      if(ndims==2)
        property = new ElementProperty<real_t>(_mesh->get_coords(n[0]),
                                               _mesh->get_coords(n[1]),
                                               _mesh->get_coords(n[2]));
      else
        property = new ElementProperty<real_t>(_mesh->get_coords(n[0]),
                                               _mesh->get_coords(n[1]),
                                               _mesh->get_coords(n[2]),
                                               _mesh->get_coords(n[3]));
      break;
    }
  }
  
  /// Default destructor.
  ~Swapping(){
    delete property;
  }
  
  void swap(real_t Q_min){
    // Cache the element quality's.
    size_t NElements = _mesh->get_number_elements();
    std::vector<real_t> quality(NElements, -1);
#pragma omp parallel
    {
#pragma omp for schedule(static)
      for(int i=0;i<(int)NElements;i++){
        const int *n=_mesh->get_element(i);
        if(n[0]<0){
          quality[i] = 0.0;
          continue;
        }
        if(ndims==2){
          const real_t *x0 = _mesh->get_coords(n[0]);
          const real_t *x1 = _mesh->get_coords(n[1]);
          const real_t *x2 = _mesh->get_coords(n[2]);
          
          quality[i] = property->lipnikov(x0, x1, x2,
                                          _mesh->get_metric(n[0]),
                                          _mesh->get_metric(n[1]),
                                          _mesh->get_metric(n[2]));
        }else{
          const real_t *x0 = _mesh->get_coords(n[0]);
          const real_t *x1 = _mesh->get_coords(n[1]);
          const real_t *x2 = _mesh->get_coords(n[2]);
          const real_t *x3 = _mesh->get_coords(n[3]);
          
          quality[i] = property->lipnikov(x0, x1, x2, x3,
                                          _mesh->get_metric(n[0]),
                                          _mesh->get_metric(n[1]),
                                          _mesh->get_metric(n[2]),
                                          _mesh->get_metric(n[3]));
        }
      }
    }

    if(ndims==2){
#if 1
      // Initialise list of dynamic edges.
      typename std::vector< std::vector<char> > marked_edges(_mesh->NNList.size());
      index_t n_marked_edges = 0;
      originalVertexDegree.clear();
      originalVertexDegree.resize(_mesh->NNList.size(), (size_t) 0);
      typename std::vector< std::vector<index_t> > NEList(_mesh->NEList.size());

      #pragma omp parallel for schedule(static) reduction(+:n_marked_edges)
      for(int i=0;i<(int)_mesh->NNList.size();i++){
        size_t size = _mesh->NNList[i].size();
        if(size == 0)
          continue;

        originalVertexDegree[i] = size;
        _mesh->NNList[i].resize(3 * size, (index_t) -1);
        marked_edges[i].resize(size, (char) 0);
        NEList[i].resize(2 * size, (index_t) -1);
        std::copy(_mesh->NEList[i].begin(), _mesh->NEList[i].end(), NEList[i].begin());

        for(int it=0; it<(int)size; ++it){
          if(i < _mesh->NNList[i][it]){
            marked_edges[i][it] = 1;
            ++n_marked_edges;
          }
        }
      }

      // -
      while(n_marked_edges > 0){
        n_marked_edges = 0;

        #pragma omp parallel
        {
          #pragma omp for schedule(dynamic)
          for(int i=0;i<(int)_mesh->NNList.size();i++){
            if(_mesh->is_halo_node(i)){
           	  fill(marked_edges[i].begin(), marked_edges[i].end(), (char) 0);
              continue;
            }

            for(int it=0; it<(int)originalVertexDegree[i]; ++it){
              if(marked_edges[i][it] != 1)
                continue;

              index_t opposite = _mesh->NNList[i][it];

              if(_mesh->is_halo_node(opposite)){
                marked_edges[i][it] = 0;
                continue;
              }

              // Find the two elements sharing this edge
              std::vector<index_t> neigh_elements;
              for(size_t k=0; k<NEList[i].size()/2; ++k){
                if(NEList[i][k] != -1)
                  for(size_t l=0; l<NEList[opposite].size()/2; ++l)
                    if(NEList[i][k] == NEList[opposite][l])
                      neigh_elements.push_back(NEList[i][k]);
              }

              if(neigh_elements.size()!=2){
                marked_edges[i][it] = 0;
                continue;
              }

              int eid0 = *neigh_elements.begin();
              int eid1 = *neigh_elements.rbegin();

              /*
              if(std::min(quality[eid0], quality[eid1])>Q_min)
                continue;
              */

              const int *n = _mesh->get_element(eid0);
              const int *m = _mesh->get_element(eid1);

              int n_off=-1;
              for(size_t k=0;k<3;k++){
                if((n[k]!=i) && (n[k]!=opposite)){
                  n_off = k;
                  break;
                }
              }

              int m_off=-1;
              for(size_t k=0;k<3;k++){
                if((m[k]!=i) && (m[k]!=opposite)){
                  m_off = k;
                  break;
                }
              }

              //
              // Decision algorithm
              //

              /*
               * If the following condition is true, it means that this thread had
               * a stale view of NEList and ENList, which in turn means that another
               * thread performed swapping on one of the lateral edges, so anyway
               * this edge would not be a candidate for swapping during this round.
               */
              if(n_off<0 || m_off<0 || n[(n_off+2)%3]!=m[(m_off+1)%3] || n[(n_off+1)%3]!=m[(m_off+2)%3])
                continue;

              index_t lateral_n = n[n_off];
              index_t lateral_m = m[m_off];

              // i's index in lateral_n's and lateral_m's list
              int idx_in_n = -1, idx_in_m = -1;
              // lateral_n's and lateral_m's index in i's list
              int idx_of_n = -1, idx_of_m = -1;
              // Min and max ID between opposite and lateral_n, max's index in min's list
              int min_opp_n = -1, max_opp_n = -1, idx_opp_n = -1;
              // Min and max ID between opposite and lateral_m, max's index in min's list
              int min_opp_m = -1, max_opp_m = -1, idx_opp_m = -1;

              /*
               * Are lateral edges marked for processing?
               * (This also checks whether the four participating
               * vertices are original neighbours of one another)
               */
              if(i > lateral_n){
                idx_in_n = originalNeighborIndex(lateral_n, i);
                if(idx_in_n >= (int) originalVertexDegree[lateral_n])
                  continue;
                if(marked_edges[lateral_n][idx_in_n] == 1)
                  continue;

                if(opposite < lateral_n){
                  min_opp_n = opposite;
                  max_opp_n = lateral_n;
                }else{
                  min_opp_n = lateral_n;
                  max_opp_n = opposite;
                }

                idx_opp_n = originalNeighborIndex(min_opp_n, max_opp_n);
                if(idx_opp_n >= (int) originalVertexDegree[min_opp_n])
                  continue;
                if(marked_edges[min_opp_n][idx_opp_n] == 1)
                  continue;
              }

              if(i > lateral_m){
                idx_in_m = originalNeighborIndex(lateral_m, i);
                if(idx_in_m >= (int) originalVertexDegree[lateral_m])
                  continue;
                if(marked_edges[lateral_m][idx_in_m] == 1)
                  continue;

                if(opposite < lateral_m){
                  min_opp_m = opposite;
                  max_opp_m = lateral_m;
                }else{
                  min_opp_m = lateral_m;
                  max_opp_m = opposite;
                }

                idx_opp_m = originalNeighborIndex(min_opp_m, max_opp_m);
                if(idx_opp_m >= (int) originalVertexDegree[min_opp_m])
                  continue;
                if(marked_edges[min_opp_m][idx_opp_m] == 1)
                  continue;
              }

              /*
               * Are lateral neighbours original ones?
               * (only perform this check if it wasn't
               * performed during the previous decision block)
               */
              if(idx_in_n == -1){
                idx_of_n = originalNeighborIndex(i, lateral_n);
                if(idx_of_n >= (int) originalVertexDegree[i])
                  continue;
              }

              if(idx_in_m == -1){
                idx_of_m = originalNeighborIndex(i, lateral_m);
                if(idx_of_m >= (int) originalVertexDegree[i])
                  continue;
              }

              if(idx_opp_n == -1){
                if(opposite < lateral_n){
                  min_opp_n = opposite;
                  max_opp_n = lateral_n;
                }else{
                  min_opp_n = lateral_n;
                  max_opp_n = opposite;
                }

                idx_opp_n = originalNeighborIndex(min_opp_n, max_opp_n);
                if(idx_opp_n >= (int) originalVertexDegree[min_opp_n])
                  continue;
              }

              if(idx_opp_m == -1){
                if(opposite < lateral_m){
                  min_opp_m = opposite;
                  max_opp_m = lateral_m;
                }else{
                  min_opp_m = lateral_m;
                  max_opp_m = opposite;
                }

                idx_opp_m = originalNeighborIndex(min_opp_m, max_opp_m);
                if(idx_opp_m >= (int) originalVertexDegree[min_opp_m])
                  continue;
              }

              // If execution reaches this point, it means that the edge can be processed

              int n_swap[] = {n[n_off], m[m_off],       n[(n_off+2)%3]}; // new eid0
              int m_swap[] = {n[n_off], n[(n_off+1)%3], m[m_off]};       // new eid1

              real_t worst_q = std::min(quality[eid0], quality[eid1]);
              real_t q0 = property->lipnikov(_mesh->get_coords(n_swap[0]),
                                             _mesh->get_coords(n_swap[1]),
                                             _mesh->get_coords(n_swap[2]),
                                             _mesh->get_metric(n_swap[0]),
                                             _mesh->get_metric(n_swap[1]),
                                             _mesh->get_metric(n_swap[2]));
              real_t q1 = property->lipnikov(_mesh->get_coords(m_swap[0]),
                                             _mesh->get_coords(m_swap[1]),
                                             _mesh->get_coords(m_swap[2]),
                                             _mesh->get_metric(m_swap[0]),
                                             _mesh->get_metric(m_swap[1]),
                                             _mesh->get_metric(m_swap[2]));
              real_t new_worst_q = std::min(q0, q1);

              if(new_worst_q>worst_q){
                // Cache new quality measures.
                quality[eid0] = q0;
                quality[eid1] = q1;

                //
                // Update NNList[i], NNList[opposite], NNList[lateral_n] and NNList[lateral_m]
                //

                // Remove opposite from i's list
                _mesh->NNList[i][it] = -1;

                // Remove i from opposite's list
                _mesh->NNList[opposite][originalNeighborIndex(opposite, i)] = -1;

                // Add lateral_m in lateral_n's list
                if(idx_in_n == -1)
                  idx_in_n = originalNeighborIndex(lateral_n, i);
                int pos = originalVertexDegree[lateral_n] + idx_in_n;
                if(_mesh->NNList[lateral_n][pos] != -1)
                  pos += originalVertexDegree[lateral_n];
                assert(_mesh->NNList[lateral_n][pos] == -1);
                _mesh->NNList[lateral_n][pos] = lateral_m;

                // Add lateral_n in lateral_m's list
                if(idx_in_m == -1)
                  idx_in_m = originalNeighborIndex(lateral_m, i);
                pos = originalVertexDegree[lateral_m] + idx_in_m;
                if(_mesh->NNList[lateral_m][pos] != -1)
                  pos += originalVertexDegree[lateral_m];
                assert(_mesh->NNList[lateral_m][pos] == -1);
                _mesh->NNList[lateral_m][pos] = lateral_n;

                //
                // Update node-element list.
                //

                // Erase old node-element adjacency.
                index_t vertex;
                size_t halfSize;
                typename std::vector<index_t>::iterator it;

                // lateral_n - add eid1
                vertex = n_swap[0];
                halfSize = NEList[vertex].size()/2;
                it = std::find(NEList[vertex].begin(), NEList[vertex].begin() + halfSize, eid0);
                assert(it != NEList[vertex].begin() + halfSize);
                it += halfSize;
                assert(*it == -1);
                *it = eid1;

                // lateral_m - add eid0
                vertex = n_swap[1];
                halfSize = NEList[vertex].size()/2;
                it = std::find(NEList[vertex].begin(), NEList[vertex].begin() + halfSize, eid1);
                assert(it != NEList[vertex].begin() + halfSize);
                it += halfSize;
                assert(*it == -1);
                *it = eid0;

                // i (or opposite) - remove eid1
                vertex = n_swap[2];
                halfSize = NEList[vertex].size()/2;
                it = std::find(NEList[vertex].begin(), NEList[vertex].begin() + halfSize, eid1);
                assert(it != NEList[vertex].begin() + halfSize);
                assert(*it == eid1);
                *it = -1;

                // opposite (or i) - remove eid0
                vertex = m_swap[1];
                halfSize = NEList[vertex].size()/2;
                it = std::find(NEList[vertex].begin(), NEList[vertex].begin() + halfSize, eid0);
                assert(it != NEList[vertex].begin() + halfSize);
                assert(*it == eid0);
                *it = -1;

                // Update element-node list for this element.
                for(size_t k=0;k<nloc;k++){
                  _mesh->_ENList[eid0*nloc+k] = n_swap[k];
                  _mesh->_ENList[eid1*nloc+k] = m_swap[k];
                }

                // Also update the edges that have to be rechecked.
                if(i < lateral_n)
                  marked_edges[i][idx_of_n] = 1;
                else
                  marked_edges[lateral_n][idx_in_n] = 1;

                if(i < lateral_m)
                  marked_edges[i][idx_of_m] = 1;
                else
                  marked_edges[lateral_m][idx_in_m] = 1;

                assert(idx_opp_n!=-1);
                assert(idx_opp_m!=-1);
                marked_edges[min_opp_n][idx_opp_n] = 1;
                marked_edges[min_opp_m][idx_opp_m] = 1;
              }

              // Mark the swapped edge as processed
              marked_edges[i][it] = 0;
            }
          }

          #pragma omp for schedule(dynamic) reduction(+:n_marked_edges)
          for(int i=0;i<(int)_mesh->NNList.size();i++){
            n_marked_edges += std::count(marked_edges[i].begin(), marked_edges[i].end(), (char) 1);
          }

          /*
           * This is used to determine whether swapping is finished.
           * If this is the case, NNList[i] needs not be resized x3.
           * Same for NEList.
           */
          int NNextend = (n_marked_edges > 0 ? 3 : 1);
          int NEextend = (n_marked_edges > 0 ? 2 : 1);

          // Compact NNList, NEList
          #pragma omp for schedule(dynamic)
          for(int i=0;i<(int)_mesh->NNList.size();i++){
            if(_mesh->NNList[i].size() == 0)
              continue;

            size_t forward = 0, backward = _mesh->NNList[i].size() - 1;

            while(forward < backward){
              while(_mesh->NNList[i][forward] != -1) ++forward;
              while(_mesh->NNList[i][backward] == -1) --backward;

              if(forward < backward){
                _mesh->NNList[i][forward] = _mesh->NNList[i][backward];
                _mesh->NNList[i][backward] = -1;
                if(backward < originalVertexDegree[i])
                  marked_edges[i][forward] = marked_edges[i][backward];
              }
              else
                break;

              ++forward;
              --backward;
            }
            if(_mesh->NNList[i][forward] != -1)
              ++forward;

            originalVertexDegree[i] = forward;
            marked_edges[i].resize(forward, (char) 0);
            _mesh->NNList[i].resize(NNextend*forward, (index_t) -1);

            forward = 0, backward = NEList[i].size() - 1;

            while(forward < backward){
              while(NEList[i][forward] != -1){
                ++forward;
                if(forward>backward)
                  break;
              }
              while(NEList[i][backward] == -1){
                --backward;
                if(forward>backward)
                  break;
              }

              if(forward < backward){
                NEList[i][forward] = NEList[i][backward];
                NEList[i][backward] = -1;
              }
              else
                break;

              ++forward;
              --backward;
            }
            if(NEList[i][forward] != -1)
              ++forward;

            assert((size_t)i<NEList.size());
            NEList[i].resize(NEextend*forward, (index_t) -1);
          }
        }
      }

      #pragma omp parallel for schedule(dynamic)
      for(int i=0;i<(int)_mesh->NNList.size();i++){
        if(_mesh->NEList[i].empty())
          continue;

        _mesh->NEList[i].clear();
        std::copy(NEList[i].begin(), NEList[i].end(), std::inserter(_mesh->NEList[i], _mesh->NEList[i].begin()));
      }
#else
      // Initialise list of dynamic edges.
      typename std::set<Edge<index_t> > dynamic_edges;
      for(int i=0;i<(int)_mesh->NNList.size();i++){
        for(typename std::deque<index_t>::const_iterator it=_mesh->NNList[i].begin();it!=_mesh->NNList[i].end();++it){
          if(i<*it){
            std::set<index_t> neigh_elements;
            set_intersection(_mesh->NEList[i].begin(), _mesh->NEList[i].end(),
                             _mesh->NEList[*it].begin(), _mesh->NEList[*it].end(),
                             inserter(neigh_elements, neigh_elements.begin()));

            if(neigh_elements.size()!=2)
              continue;

            for(typename std::set<index_t>::const_iterator jt=neigh_elements.begin();jt!=neigh_elements.end();++jt){
              if(quality[*jt]<Q_min){
                dynamic_edges.insert(Edge<index_t>(i, *it));
                break;
              }
            }
          }
        }
      }

      // -
      while(!dynamic_edges.empty()){
        Edge<index_t> target_edge = *dynamic_edges.begin();
        dynamic_edges.erase(dynamic_edges.begin());

        std::set<index_t> neigh_elements;
        set_intersection(_mesh->NEList[target_edge.edge.first].begin(), _mesh->NEList[target_edge.edge.first].end(),
                         _mesh->NEList[target_edge.edge.second].begin(), _mesh->NEList[target_edge.edge.second].end(),
                         inserter(neigh_elements, neigh_elements.begin()));

        if(neigh_elements.size()!=2)
          continue;

        if(_mesh->is_halo_node(target_edge.edge.first) || _mesh->is_halo_node(target_edge.edge.second))
          continue;

        int eid0 = *neigh_elements.begin();
        int eid1 = *neigh_elements.rbegin();

        if(std::min(quality[eid0], quality[eid1])>Q_min)
          continue;

        const int *n = _mesh->get_element(eid0);
        const int *m = _mesh->get_element(eid1);

        int n_off=-1;
        for(size_t i=0;i<3;i++){
          if((n[i]!=target_edge.edge.first) && (n[i]!=target_edge.edge.second)){
            n_off = i;
            break;
          }
        }
        assert(n_off>=0);

        int m_off=-1;
        for(size_t i=0;i<3;i++){
          if((m[i]!=target_edge.edge.first) && (m[i]!=target_edge.edge.second)){
            m_off = i;
            break;
          }
        }
        assert(m_off>=0);

        assert(n[(n_off+2)%3]==m[(m_off+1)%3]);
        assert(n[(n_off+1)%3]==m[(m_off+2)%3]);

        int n_swap[] = {n[n_off], m[m_off],       n[(n_off+2)%3]}; // new eid0
        int m_swap[] = {n[n_off], n[(n_off+1)%3], m[m_off]};   // new eid1

        real_t worst_q = std::min(quality[eid0], quality[eid1]);
        real_t q0 = property->lipnikov(_mesh->get_coords(n_swap[0]),
                                       _mesh->get_coords(n_swap[1]),
                                       _mesh->get_coords(n_swap[2]),
                                       _mesh->get_metric(n_swap[0]),
                                       _mesh->get_metric(n_swap[1]),
                                       _mesh->get_metric(n_swap[2]));
        real_t q1 = property->lipnikov(_mesh->get_coords(m_swap[0]),
                                       _mesh->get_coords(m_swap[1]),
                                       _mesh->get_coords(m_swap[2]),
                                       _mesh->get_metric(m_swap[0]),
                                       _mesh->get_metric(m_swap[1]),
                                       _mesh->get_metric(m_swap[2]));
        real_t new_worst_q = std::min(q0, q1);

        if(new_worst_q>worst_q){
          // Cache new quality measures.
          quality[eid0] = q0;
          quality[eid1] = q1;

          //
          // Update node-node list.
          //

          // Make local partial copy of nnlist
          std::map<int, std::set<int> > nnlist;
          for(size_t i=0;i<nloc;i++){
            for(typename std::deque<index_t>::const_iterator it=_mesh->NNList[n[i]].begin();it!=_mesh->NNList[n[i]].end();++it){
              if(*it>=0)
                nnlist[n[i]].insert(*it);
            }
          }
          for(typename std::deque<index_t>::const_iterator it=_mesh->NNList[m[m_off]].begin();it!=_mesh->NNList[m[m_off]].end();++it){
            if(*it>=0)
              nnlist[m[m_off]].insert(*it);
          }
          nnlist[n[(n_off+1)%3]].erase(n[(n_off+2)%3]);
          nnlist[n[(n_off+2)%3]].erase(n[(n_off+1)%3]);

          nnlist[n[n_off]].insert(m[m_off]);
          nnlist[m[m_off]].insert(n[n_off]);

          // Put back in new adjacency info
          for(std::map<int, std::set<int> >::const_iterator it=nnlist.begin();it!=nnlist.end();++it){
            _mesh->NNList[it->first].clear();
            for(typename std::set<index_t>::const_iterator jt=it->second.begin();jt!=it->second.end();++jt)
              _mesh->NNList[it->first].push_back(*jt);
          }

          //
          // Update node-element list.
          //

          // Erase old node-element adjacency.
          for(size_t i=0;i<nloc;i++){
            _mesh->NEList[n[i]].erase(eid0);
            _mesh->NEList[m[i]].erase(eid1);
          }
          for(size_t i=0;i<nloc;i++){
            _mesh->NEList[n_swap[i]].insert(eid0);
            _mesh->NEList[m_swap[i]].insert(eid1);
          }

          // Update element-node list for this element.
          for(size_t i=0;i<nloc;i++){
            _mesh->_ENList[eid0*nloc+i] = n_swap[i];
            _mesh->_ENList[eid1*nloc+i] = m_swap[i];
          }

          // Also update the edges that have to be rechecked.
          dynamic_edges.insert(Edge<index_t>(n_swap[0], n_swap[2]));
          dynamic_edges.insert(Edge<index_t>(n_swap[1], n_swap[2]));
          dynamic_edges.insert(Edge<index_t>(m_swap[0], m_swap[1]));
          dynamic_edges.insert(Edge<index_t>(m_swap[1], m_swap[2]));
        }
      }
#endif
    }else{
      assert(ndims==3);
      std::map<int, std::deque<int> > partialEEList;
      for(size_t i=0;i<NElements;i++){
        // Check this is not deleted.
        const int *n=_mesh->get_element(i);
        if(n[0]<0)
          continue;
        
        // Only start storing information for poor elements.
        if(quality[i]<Q_min){
          partialEEList[i].resize(4);
          fill(partialEEList[i].begin(), partialEEList[i].end(), -1);
          
          for(size_t j=0;j<4;j++){
            std::set<index_t> intersection12;
            set_intersection(_mesh->NEList[n[(j+1)%4]].begin(), _mesh->NEList[n[(j+1)%4]].end(),
                             _mesh->NEList[n[(j+2)%4]].begin(), _mesh->NEList[n[(j+2)%4]].end(),
                             inserter(intersection12, intersection12.begin()));

            std::set<index_t> EE;
            set_intersection(intersection12.begin(),intersection12.end(),
                             _mesh->NEList[n[(j+3)%4]].begin(), _mesh->NEList[n[(j+3)%4]].end(),
                             inserter(EE, EE.begin()));
            
            for(typename std::set<index_t>::const_iterator it=EE.begin();it!=EE.end();++it){
              if(*it != (index_t)i){
                partialEEList[i][j] = *it;
                break;
              }
            }
          }
        }
      }

      // Colour the graph and choose the maximal independent set.
      std::map<int , std::set<int> > graph;
      for(std::map<int, std::deque<int> >::const_iterator it=partialEEList.begin();it!=partialEEList.end();++it){
        for(std::deque<int>::const_iterator jt=it->second.begin();jt!=it->second.end();++jt){
          graph[*jt].insert(it->first);
          graph[it->first].insert(*jt);
        }
      }

      std::deque<int> renumber(graph.size());
      std::map<int, int> irenumber;
      // std::vector<size_t> nedges(graph.size());
      size_t loc=0;
      for(std::map<int , std::set<int> >::const_iterator it=graph.begin();it!=graph.end();++it){
        // nedges[loc] = it->second.size();
        renumber[loc] = it->first;
        irenumber[it->first] = loc;
        loc++;
      }
      
      std::vector< std::deque<index_t> > NNList(graph.size());
      for(std::map<int , std::set<int> >::const_iterator it=graph.begin();it!=graph.end();++it){
        for(std::set<int>::const_iterator jt=it->second.begin();jt!=it->second.end();++jt){
          NNList[irenumber[it->first]].push_back(irenumber[*jt]);
        }
      }
      std::vector<index_t> colour(graph.size());
      Colour<index_t>::greedy(NNList, &(colour[0]));

      /*
      std::vector<size_t> csr_edges;
      for(std::map<int , std::set<int> >::const_iterator it=graph.begin();it!=graph.end();++it){
        for(std::set<int>::const_iterator jt=it->second.begin();jt!=it->second.end();++jt){
          csr_edges.push_back(*jt);
        }
      }

      zoltan_graph_t flat_graph;
      flat_graph.rank=0;
      flat_graph.npnodes = graph.size();
      flat_graph.nnodes = graph.size();
      flat_graph.nedges = &(nedges[0]);
      flat_graph.csr_edges = &(csr_edges[0]);
      std::vector<int> gid(flat_graph.nnodes);
      for(int i=0;i<flat_graph.nnodes;i++)
        gid[i] = i;
      flat_graph.gid = &(gid[0]);
      std::vector<size_t> owner(flat_graph.nnodes, 0);
      std::vector<int> colour(flat_graph.nnodes);

      zoltan_colour(&flat_graph, 2, MPI_COMM_NULL);

      */

      // Assume colour 0 will be the maximal independent set.
      
      int max_colour=colour[0];
      for(size_t i=1;i<graph.size();i++)
        max_colour = std::max(max_colour, colour[i]);
      
      // Process face-to-edge swap.
      for(int c=0;c<max_colour;c++)
        for(size_t i=0;i<graph.size();i++){
          int eid0 = renumber[i];
          
          if(colour[i]==c && (partialEEList.count(eid0)>0)){
            
            // Check this is not deleted.
            const int *n=_mesh->get_element(eid0);
            if(n[0]<0)
              continue;
            
            assert(partialEEList[eid0].size()==4);
            
            // Check adjacency is not toxic.
            bool toxic = false;
            for(int j=0;j<4;j++){
              int eid1 = partialEEList[eid0][j];
              if(eid1==-1)
                continue;
              
              const int *m=_mesh->get_element(eid1);
              if(m[0]<0){
                toxic = true;
                break;
              }
            }
            if(toxic)
              continue;
            
            // Create set of nodes for quick lookup.
            std::set<int> ele0_set;
            for(int j=0;j<4;j++)
              ele0_set.insert(n[j]);
            
            for(int j=0;j<4;j++){  
              int eid1 = partialEEList[eid0][j];
              if(eid1==-1)
                continue;
              
              std::vector<int> hull(5, -1);
              if(j==0){
                hull[0] = n[1];
                hull[1] = n[3];
                hull[2] = n[2];
                hull[3] = n[0];
              }else if(j==1){
                hull[0] = n[2];
                hull[1] = n[3];
                hull[2] = n[0];
                hull[3] = n[1];
              }else if(j==2){
                hull[0] = n[0];
                hull[1] = n[3];
                hull[2] = n[1];
                hull[3] = n[2];
              }else if(j==3){
                hull[0] = n[0];
                hull[1] = n[1];
                hull[2] = n[2];
                hull[3] = n[3];
              }
              
              const int *m=_mesh->get_element(eid1);
              assert(m[0]>=0);
              
              for(int k=0;k<4;k++)
                if(ele0_set.count(m[k])==0){
                  hull[4] = m[k];
                  break;
                }
              assert(hull[4]!=-1);
              
              // New element: 0143
              real_t q0 = property->lipnikov(_mesh->get_coords(hull[0]),
                                             _mesh->get_coords(hull[1]),
                                             _mesh->get_coords(hull[4]),
                                             _mesh->get_coords(hull[3]),
                                             _mesh->get_metric(hull[0]),
                                             _mesh->get_metric(hull[1]),
                                             _mesh->get_metric(hull[4]),
                                             _mesh->get_metric(hull[3]));
              
              // New element: 1243
              real_t q1 = property->lipnikov(_mesh->get_coords(hull[1]),
                                             _mesh->get_coords(hull[2]),
                                             _mesh->get_coords(hull[4]),
                                             _mesh->get_coords(hull[3]),
                                             _mesh->get_metric(hull[1]),
                                             _mesh->get_metric(hull[2]),
                                             _mesh->get_metric(hull[4]),
                                             _mesh->get_metric(hull[3]));
              
              // New element:2043
              real_t q2 = property->lipnikov(_mesh->get_coords(hull[2]),
                                             _mesh->get_coords(hull[0]),
                                             _mesh->get_coords(hull[4]),
                                             _mesh->get_coords(hull[3]),
                                             _mesh->get_metric(hull[2]),
                                             _mesh->get_metric(hull[0]),
                                             _mesh->get_metric(hull[4]),
                                             _mesh->get_metric(hull[3]));
              
              if(std::min(quality[eid0],quality[eid1]) < std::min(q0, std::min(q1, q2))){
                _mesh->erase_element(eid0);
                _mesh->erase_element(eid1);
                
                int e0[] = {hull[0], hull[1], hull[4], hull[3]};
                _mesh->append_element(e0);
                quality.push_back(q0);
                
                int e1[] = {hull[1], hull[2], hull[4], hull[3]};
                _mesh->append_element(e1);
                quality.push_back(q1);
                
                int e2[] = {hull[2], hull[0], hull[4], hull[3]};
                _mesh->append_element(e2);
                quality.push_back(q2);
                
                break;
              }
            }
          }
        }
      
      // Process edge-face swaps.
      for(int c=0;c<max_colour;c++)
        for(size_t i=0;i<graph.size();i++){
          int eid0 = renumber[i];
          
          if(colour[i]==c && (partialEEList.count(eid0)>0)){
            
            // Check this is not deleted.
            const int *n=_mesh->get_element(eid0);
            if(n[0]<0)
              continue;
            
            bool toxic=false, swapped=false;
            for(int k=0;(k<3)&&(!toxic)&&(!swapped);k++){
              for(int l=k+1;l<4;l++){                 
                Edge<index_t> edge = Edge<index_t>(n[k], n[l]);
                
                std::set<index_t> neigh_elements;
                set_intersection(_mesh->NEList[n[k]].begin(), _mesh->NEList[n[k]].end(),
                                 _mesh->NEList[n[l]].begin(), _mesh->NEList[n[l]].end(),
                                 inserter(neigh_elements, neigh_elements.begin()));
                
                double min_quality = quality[eid0];
                std::vector<index_t> constrained_edges_unsorted;
                for(typename std::set<index_t>::const_iterator it=neigh_elements.begin();it!=neigh_elements.end();++it){
                  min_quality = std::min(min_quality, quality[*it]);
                  
                  const int *m=_mesh->get_element(*it);
                  if(m[0]<0){
                    toxic=true;
                    break;
                  }
                  
                  for(int j=0;j<4;j++){
                    if((m[j]!=n[k])&&(m[j]!=n[l])){
                      constrained_edges_unsorted.push_back(m[j]);
                    }
                  }
                }
                
                if(toxic)
                  break;
                
                size_t nelements = neigh_elements.size();
                assert(nelements*2==constrained_edges_unsorted.size());
                
                // Sort edges.
                std::vector<index_t> constrained_edges;
                std::vector<bool> sorted(nelements, false);
                constrained_edges.push_back(constrained_edges_unsorted[0]);
                constrained_edges.push_back(constrained_edges_unsorted[1]);
                for(size_t j=1;j<nelements;j++){
                  for(size_t e=1;e<nelements;e++){
                    if(sorted[e])
                      continue;
                    if(*constrained_edges.rbegin()==constrained_edges_unsorted[e*2]){
                      constrained_edges.push_back(constrained_edges_unsorted[e*2]);
                      constrained_edges.push_back(constrained_edges_unsorted[e*2+1]);
                      sorted[e]=true;
                      break;
                    }else if(*constrained_edges.rbegin()==constrained_edges_unsorted[e*2+1]){
                      constrained_edges.push_back(constrained_edges_unsorted[e*2+1]);
                      constrained_edges.push_back(constrained_edges_unsorted[e*2]);
                      sorted[e]=true;
                      break;
                    }
                  }
                }
                
                if(*constrained_edges.begin() != *constrained_edges.rbegin()){
                  assert(_surface->contains_node(n[k]));
                  assert(_surface->contains_node(n[l]));
                  
                  toxic = true;
                  break;
                }
                
                std::vector< std::vector<index_t> > new_elements;
                if(nelements==3){
                  // This is the 3-element to 2-element swap.
                  new_elements.resize(1);
                  
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(n[l]);
                  
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(n[k]);
                }else if(nelements==4){
                  // This is the 4-element to 4-element swap.
                  new_elements.resize(2);

                  // Option 1.
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(n[l]);
                  
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(n[l]);

                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(n[k]);
                                    
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(n[k]);

                  // Option 2
                  new_elements[1].push_back(constrained_edges[0]);
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(constrained_edges[4]);
                  new_elements[1].push_back(n[l]);
                  
                  new_elements[1].push_back(constrained_edges[0]);
                  new_elements[1].push_back(constrained_edges[4]);
                  new_elements[1].push_back(constrained_edges[6]);
                  new_elements[1].push_back(n[l]);

                  new_elements[1].push_back(constrained_edges[0]);
                  new_elements[1].push_back(constrained_edges[4]);
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(n[k]);
                                    
                  new_elements[1].push_back(constrained_edges[0]);
                  new_elements[1].push_back(constrained_edges[6]);
                  new_elements[1].push_back(constrained_edges[4]);
                  new_elements[1].push_back(n[k]);
                }else if(nelements==5){
                  // This is the 5-element to 6-element swap.
                  new_elements.resize(5);

                  // Option 1
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(n[l]);
                  
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(n[l]);
                  
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(constrained_edges[8]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(n[l]);
                  
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(n[k]);
                  
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(n[k]);
                  
                  new_elements[0].push_back(constrained_edges[8]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(n[k]);

                  // Option 2
                  new_elements[1].push_back(constrained_edges[0]);
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(constrained_edges[8]);
                  new_elements[1].push_back(n[l]);
                  
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(constrained_edges[6]);
                  new_elements[1].push_back(constrained_edges[8]);
                  new_elements[1].push_back(n[l]);
                  
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(constrained_edges[4]);
                  new_elements[1].push_back(constrained_edges[6]);
                  new_elements[1].push_back(n[l]);
                  
                  new_elements[1].push_back(constrained_edges[0]);
                  new_elements[1].push_back(constrained_edges[8]);
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(n[k]);
                  
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(constrained_edges[8]);
                  new_elements[1].push_back(constrained_edges[6]);
                  new_elements[1].push_back(n[k]);
                  
                  new_elements[1].push_back(constrained_edges[2]);
                  new_elements[1].push_back(constrained_edges[6]);
                  new_elements[1].push_back(constrained_edges[4]);
                  new_elements[1].push_back(n[k]);

                  // Option 3
                  new_elements[2].push_back(constrained_edges[4]);
                  new_elements[2].push_back(constrained_edges[0]);
                  new_elements[2].push_back(constrained_edges[2]);
                  new_elements[2].push_back(n[l]);
                  
                  new_elements[2].push_back(constrained_edges[4]);
                  new_elements[2].push_back(constrained_edges[8]);
                  new_elements[2].push_back(constrained_edges[0]);
                  new_elements[2].push_back(n[l]);
                  
                  new_elements[2].push_back(constrained_edges[4]);
                  new_elements[2].push_back(constrained_edges[6]);
                  new_elements[2].push_back(constrained_edges[8]);
                  new_elements[2].push_back(n[l]);
                  
                  new_elements[2].push_back(constrained_edges[4]);
                  new_elements[2].push_back(constrained_edges[2]);
                  new_elements[2].push_back(constrained_edges[0]);
                  new_elements[2].push_back(n[k]);
                  
                  new_elements[2].push_back(constrained_edges[4]);
                  new_elements[2].push_back(constrained_edges[0]);
                  new_elements[2].push_back(constrained_edges[8]);
                  new_elements[2].push_back(n[k]);
                  
                  new_elements[2].push_back(constrained_edges[4]);
                  new_elements[2].push_back(constrained_edges[8]);
                  new_elements[2].push_back(constrained_edges[6]);
                  new_elements[2].push_back(n[k]);

                  // Option 4
                  new_elements[3].push_back(constrained_edges[6]);
                  new_elements[3].push_back(constrained_edges[2]);
                  new_elements[3].push_back(constrained_edges[4]);
                  new_elements[3].push_back(n[l]);
                  
                  new_elements[3].push_back(constrained_edges[6]);
                  new_elements[3].push_back(constrained_edges[0]);
                  new_elements[3].push_back(constrained_edges[2]);
                  new_elements[3].push_back(n[l]);
                  
                  new_elements[3].push_back(constrained_edges[6]);
                  new_elements[3].push_back(constrained_edges[8]);
                  new_elements[3].push_back(constrained_edges[0]);
                  new_elements[3].push_back(n[l]);
                  
                  new_elements[3].push_back(constrained_edges[6]);
                  new_elements[3].push_back(constrained_edges[4]);
                  new_elements[3].push_back(constrained_edges[2]);
                  new_elements[3].push_back(n[k]);
                  
                  new_elements[3].push_back(constrained_edges[6]);
                  new_elements[3].push_back(constrained_edges[2]);
                  new_elements[3].push_back(constrained_edges[0]);
                  new_elements[3].push_back(n[k]);
                  
                  new_elements[3].push_back(constrained_edges[6]);
                  new_elements[3].push_back(constrained_edges[0]);
                  new_elements[3].push_back(constrained_edges[8]);
                  new_elements[3].push_back(n[k]);

                  // Option 5
                  new_elements[4].push_back(constrained_edges[8]);
                  new_elements[4].push_back(constrained_edges[0]);
                  new_elements[4].push_back(constrained_edges[2]);
                  new_elements[4].push_back(n[l]);
                  
                  new_elements[4].push_back(constrained_edges[8]);
                  new_elements[4].push_back(constrained_edges[2]);
                  new_elements[4].push_back(constrained_edges[4]);
                  new_elements[4].push_back(n[l]);
                  
                  new_elements[4].push_back(constrained_edges[8]);
                  new_elements[4].push_back(constrained_edges[4]);
                  new_elements[4].push_back(constrained_edges[6]);
                  new_elements[4].push_back(n[l]);
                  
                  new_elements[4].push_back(constrained_edges[8]);
                  new_elements[4].push_back(constrained_edges[2]);
                  new_elements[4].push_back(constrained_edges[0]);
                  new_elements[4].push_back(n[k]);
                  
                  new_elements[4].push_back(constrained_edges[8]);
                  new_elements[4].push_back(constrained_edges[4]);
                  new_elements[4].push_back(constrained_edges[2]);
                  new_elements[4].push_back(n[k]);
                  
                  new_elements[4].push_back(constrained_edges[8]);
                  new_elements[4].push_back(constrained_edges[6]);
                  new_elements[4].push_back(constrained_edges[4]);
                  new_elements[4].push_back(n[k]);
                }else if(nelements==6){
                  // This is the 6-element to 8-element swap.
                  new_elements.resize(1);

                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[10]);
                  new_elements[0].push_back(n[l]);

                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(constrained_edges[8]);
                  new_elements[0].push_back(n[l]);

                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[10]);
                  new_elements[0].push_back(n[l]);

                  new_elements[0].push_back(constrained_edges[10]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[8]);
                  new_elements[0].push_back(n[l]);

                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[0]);
                  new_elements[0].push_back(constrained_edges[10]);
                  new_elements[0].push_back(n[k]);

                  new_elements[0].push_back(constrained_edges[6]);
                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[8]);
                  new_elements[0].push_back(n[k]);

                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[2]);
                  new_elements[0].push_back(constrained_edges[10]);
                  new_elements[0].push_back(n[k]);

                  new_elements[0].push_back(constrained_edges[4]);
                  new_elements[0].push_back(constrained_edges[10]);
                  new_elements[0].push_back(constrained_edges[8]);
                  new_elements[0].push_back(n[k]);
                }else{
                  continue;
                }
                
                nelements = new_elements[0].size()/4;
                
                // Check new minimum quality.
                std::vector<double> new_min_quality(new_elements.size());
                std::vector< std::vector<double> > newq(new_elements.size());
                int best_option;
                for(int invert=0;invert<2;invert++){
                  best_option=0;
                  for(size_t option=0;option<new_elements.size();option++){
                    newq[option].resize(nelements);
                    for(size_t j=0;j<nelements;j++){
                      newq[option][j] = property->lipnikov(_mesh->get_coords(new_elements[option][j*4+0]),
                                                           _mesh->get_coords(new_elements[option][j*4+1]),
                                                           _mesh->get_coords(new_elements[option][j*4+2]),
                                                           _mesh->get_coords(new_elements[option][j*4+3]),
                                                           _mesh->get_metric(new_elements[option][j*4+0]),
                                                           _mesh->get_metric(new_elements[option][j*4+1]),
                                                           _mesh->get_metric(new_elements[option][j*4+2]),
                                                           _mesh->get_metric(new_elements[option][j*4+3]));
                    }
                    
                    new_min_quality[option] = newq[option][0];
                    for(size_t j=0;j<nelements;j++)
                      new_min_quality[option] = std::min(newq[option][j], new_min_quality[option]);
                  }
                  
                  
                  for(size_t option=1;option<new_elements.size();option++){
                    if(new_min_quality[option]>new_min_quality[best_option]){
                      best_option = option;
                    }
                  }
                  
                  if(new_min_quality[best_option] < 0.0){
                    // Invert elements.
                    for(typename std::vector< std::vector<index_t> >::iterator it=new_elements.begin();it!=new_elements.end();++it){
                      for(size_t j=0;j<nelements;j++){
                        index_t stash_id = (*it)[j*4];
                        (*it)[j*4] = (*it)[j*4+1];
                        (*it)[j*4+1] = stash_id;           
                      }
                    }

                    continue;
                  }
                  break;
                }
                                
                if(new_min_quality[best_option] <= min_quality)
                  continue;

                // Remove old elements.
                for(typename std::set<index_t>::const_iterator it=neigh_elements.begin();it!=neigh_elements.end();++it)
                  _mesh->erase_element(*it);
                
                // Add new elements.
                for(size_t j=0;j<nelements;j++){
                  _mesh->append_element(&(new_elements[best_option][j*4]));
                  quality.push_back(newq[best_option][j]);
                }
                
                swapped = true;
                break;
              }
            }
          }
        }
      // recalculate adjacency
      _mesh->create_adjancy();
    }

    return;
  }

 private:
  inline size_t originalNeighborIndex(index_t source, index_t target) const{
    size_t pos = 0;
    while(pos < originalVertexDegree[source]){
      if(_mesh->NNList[source][pos] == target)
        return pos;
      ++pos;
    }
    return std::numeric_limits<index_t>::max();
  }

  std::vector<size_t> originalVertexDegree;

  Mesh<real_t, index_t> *_mesh;
  Surface<real_t, index_t> *_surface;
  ElementProperty<real_t> *property;
  size_t ndims, nloc;
  int nthreads;
};

#endif
