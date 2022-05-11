// This code is part of the Problem Based Benchmark Suite (PBBS)
// Copyright (c) 2011 Guy Blelloch and the PBBS team
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights (to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <algorithm>
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/random.h"
#include "common/geometry.h"
#include "../utils/cluster.h"
#include <random>
#include <set>
#include <queue>
#include <math.h>

extern bool report_stats;

template<typename T>
struct pyNN_index{
	int K;
	unsigned d;
	float delta;
	using tvec_point = Tvec_point<T>;
	using fine_sequence = parlay::sequence<int>;
	// using slice_tvec = decltype(make_slice(parlay::sequence<tvec_point*>()));
	using edge = std::pair<int, int>;
	using labelled_edge = std::pair<edge, float>;
	using pid = std::pair<int, float>;
	using special_edge = std::pair<int, pid>; //this format makes it easier to use the group_by functions

	pyNN_index(int md, unsigned dim, float Delta) : K(md), d(dim), delta(Delta) {}

	static parlay::sequence<edge> naive_neighbors(parlay::sequence<tvec_point*> &v, parlay::sequence<size_t>& active_indices,
		unsigned dim, int maxK){
		size_t n = active_indices.size();
		int k = std::min(maxK, (int) (n-1));
		parlay::sequence<edge> edges(k*n);
		parlay::parallel_for(0, n, [&] (size_t i){
			parlay::sequence<labelled_edge> labelled_edges(n-1);
			parlay::parallel_for(0, n, [&] (size_t j){
				float dist = distance(v[active_indices[i]]->coordinates.begin(), v[active_indices[j]]->coordinates.begin(), dim);
				labelled_edge e = std::make_pair(std::make_pair(active_indices[i], active_indices[j]), dist);
				if(j<i) labelled_edges[j] = e;
				else if(j>i) labelled_edges[j-1] = e;
			});
			auto less = [&] (labelled_edge a, labelled_edge b) {return a.second < b.second;};
			auto sorted_edges = parlay::sort(labelled_edges);
			for(int j=0; j<k; j++) {
				edges[k*i+j] = sorted_edges[j].first;
			}
		});
		return edges;
	}

	void check_nearest_neighbors(parlay::sequence<tvec_point*> &v){
		size_t n = v.size();
		parlay::sequence<float> recall(n);
		parlay::parallel_for(0, n, [&] (size_t i) {
			auto less = [&] (pid a, pid b) {return a.second < b.second;};
			std::priority_queue<pid, std::vector<pid>, decltype(less)> neighbors(less);
			float max_dist = std::numeric_limits<float>::max();
			for(const tvec_point* p : v){
				if(p->id != i){
					float dist = distance(v[i]->coordinates.begin(), p->coordinates.begin(), d);
					if(neighbors.size() < K) neighbors.push(std::make_pair((p->id), dist));
					else if(dist < max_dist){
						neighbors.pop();
						neighbors.push(std::make_pair((p->id), dist));
						max_dist = (neighbors.top()).second;
					}
				}
			}
			parlay::sequence<int> Neighbors(K);
			for(int j=0; j<K; j++){
				Neighbors[j] = (neighbors.top()).first;
				neighbors.pop();
			}
			float num_correct = 0;
			for(const int& j : v[i]->out_nbh){
				if(parlay::find(Neighbors, j) != Neighbors.end()) num_correct ++;
			}
			recall[i] = num_correct/((float) K);
		});
		std::cout << "Total recall: " << parlay::reduce(recall)/(double (n)) << std::endl;
	}

	int generate_index(int N, int i){
		return (N*(N-1) - (N-i)*(N-i-1))/2;
	}

	parlay::sequence<int> nn_descent(parlay::sequence<tvec_point*> &v, parlay::sequence<int> &changed){
		size_t n = v.size();
		auto new_changed = parlay::tabulate(v.size(), [&] (size_t i) {return 0;});
		//find the edges in the undirected graph
		// std::cout << "here" << std::endl; 
		auto ugraph = truncated_uedges(v, changed);
		std::cout << "generated candidate edges" << std::endl;
		//tabulate all-pairs distances (possibly including duplicates) for the directed graph
		// std::cout << "here1" << std::endl; 
		parlay::sequence<parlay::sequence<special_edge>> grouped_labelled(ugraph.size());
		parlay::parallel_for(0, ugraph.size(), [&] (size_t i){
			//what if there's only one element here?????????
			// if(ugraph[i].second.size()==1) std::cout << "SIZE OF SEQUENCE = 1" << std::endl; 
			parlay::sequence<special_edge> edges = parlay::sequence<special_edge>();
			for(const int& j: ugraph[i].second){
				for(const int& k: ugraph[i].second){
					if(j!=k){
						float dist = distance(v[j]->coordinates.begin(), v[k]->coordinates.begin(), d);
						edges.push_back(std::make_pair(j, std::make_pair(k, dist)));
						edges.push_back(std::make_pair(k, std::make_pair(j, dist)));
					}
				}
			}
			grouped_labelled[i] = edges;
		});
		// std::cout << "here2" << std::endl; 
		auto flat_labelled = parlay::flatten(grouped_labelled);
		auto grouped_by = parlay::group_by_key(flat_labelled);
		// std::cout << "here3" << std::endl; 
		// update edges of each vertex based on the candidates in grouped_by
		parlay::parallel_for(0, grouped_by.size(), [&] (size_t i){
			size_t index = grouped_by[i].first;
			for(const int& j : v[index]->out_nbh) {
				// if(j>=v.size()){
				// 	std::cout << "ERROR, neighbor id larger than dataset size" << std::endl;
				// 	abort();
				// }
				float dist = distance(v[index]->coordinates.begin(), v[j]->coordinates.begin(), d);
				grouped_by[i].second.push_back(std::make_pair(j, dist));
			}
			// auto equal = [&] (pid a, pid b) {return (a.first==b.first) && (a.second==b.second);};
			
			auto less = [&] (pid a, pid b) {return a.second < b.second;};
			auto sorted_candidates = parlay::remove_duplicates_ordered(grouped_by[i].second, less);
			// auto sorted_candidates = parlay::sort(candidates);
			// for(const pid& j: sorted_candidates){
			// 	if(j.first>=v.size()){
			// 		std::cout << "ERROR, neighbor id larger than dataset size (second loop)" << std::endl;
			// 		abort();
			// 	}
			// }
			int k = std::min(K, (int) sorted_candidates.size());
			auto new_nbh = parlay::tabulate(k, [&] (size_t j) {return sorted_candidates[j].first;});
			for(const int& j: new_nbh){
				// std::cout << j << std::endl;
				if(parlay::find(v[index]->out_nbh, j) == v[index]->out_nbh.end()){
					new_changed[index] = 1;
					v[index]->new_out_nbh = new_nbh;
					break;
				} 
			}
			// std::cout << std::endl;
			// for(const int& j : v[index]->out_nbh) std::cout << j << std::endl; 

		});
		// std::cout << "here4" << std::endl;
		// std::cout << parlay::reduce(new_changed) << std::endl;  
		//finally, synchronize the new out-neighbors
		parlay::parallel_for(0, v.size(), [&] (size_t i){
			if(new_changed[i] == 1){
				v[i]->out_nbh = v[i]->new_out_nbh;
				v[i]->new_out_nbh.clear();
			}
		});
		return new_changed;
	}

	int nn_descent_wrapper(parlay::sequence<tvec_point*> &v){
		size_t n = v.size();
		auto changed = parlay::tabulate(n, [&] (size_t i) {return 1;});
		int rounds = 0;
		while(parlay::reduce(changed) >= delta*n){
			std::cout << "Round " << rounds << std::endl; 
			auto new_changed = nn_descent(v, changed);
			changed = new_changed;
			rounds ++;
			std::cout << parlay::reduce(changed) << " elements changed" << std::endl; 
		}
		std::cout << "descent converged in " << rounds << " rounds" << std::endl; 
		return rounds;
	}

	parlay::sequence<std::pair<int, parlay::sequence<int>>> truncated_uedges(parlay::sequence<tvec_point*> &v, 
		parlay::sequence<int> &changed){
		// std::cout << "there1" << std::endl; 
		auto indices = parlay::tabulate(v.size(), [&] (size_t i) {return i;});
		auto w = parlay::pack(indices, changed);
		// std::cout << "there2" << std::endl; 
		parlay::sequence<parlay::sequence<edge>> grouped_edges(w.size());
		parlay::parallel_for(0, w.size(), [&] (size_t i){
			size_t index = w[i];
			size_t m = v[index]->out_nbh.size();
			parlay::sequence<edge> undirected_edges(2*m);
			for(int j=0; j<m; j++){
				// if(v[index]->out_nbh[j] >= v.size()){
				// 	std::cout << "ERROR: edge value greater than size of dataset" << std::endl;
				// 	abort();
				// }
				undirected_edges[2*j] = std::make_pair(v[index]->out_nbh[j], (int) index);
				undirected_edges[2*j+1] = std::make_pair((int) index, v[index]->out_nbh[j]);
			}
			grouped_edges[i] = undirected_edges;
		});
		// std::cout << "there3" << std::endl; 
		auto uedges = parlay::flatten(grouped_edges);
		auto ugraph = parlay::group_by_key(uedges);
		// std::cout << "there4" << std::endl; 
		//trim the undirected edges of each graph
		parlay::parallel_for(0, ugraph.size(), [&] (size_t i){
			size_t m = ugraph[i].second.size();
			size_t index = ugraph[i].first;
			// std::cout << "there5" << std::endl; 
			// for(const int& j : ugraph[i].second){
			// 	std::cout << j << std::endl; 
			// 	std::cout << v[j]->id << std::endl;
			// }
			auto clean_edges = parlay::remove_duplicates(ugraph[i].second);
			auto labeled_edges = parlay::tabulate(clean_edges.size(), [&] (size_t j){
				auto dist = distance(v[index]->coordinates.begin(), v[clean_edges[j]]->coordinates.begin(), d);
				return std::make_pair(clean_edges[j], dist);
			});
			// std::cout << "there6" << std::endl; 
			auto less = [&] (pid a, pid b) {return a.second < b.second;};
			parlay::sort_inplace(labeled_edges, less);
			// std::cout << "there7" << std::endl; 
			int k = std::min((int) labeled_edges.size(), K*2);
			ugraph[i].second = parlay::tabulate(k, [&] (size_t j) {return labeled_edges[j].first;});
			// std::cout << "there8" << std::endl; 
		});
		// std::cout << "there9" << std::endl; 
		return ugraph;
	}

	void truncate_graph(parlay::sequence<tvec_point*> &v, int parameter){
		parlay::parallel_for(0, v.size(), [&] (size_t i) {
			auto distances = parlay::tabulate(v[i]->out_nbh.size(), [&] (size_t j){
				float dist = distance(v[i]->coordinates.begin(), v[v[i]->out_nbh[j]]->coordinates.begin(), d);
				return std::make_pair(v[i]->out_nbh[j], dist);
			});
			auto less = [&] (pid a, pid b) {return a.second < b.second;};
			auto sorted = parlay::sort(distances);
			int len = std::min((int) sorted.size(), parameter);
			fine_sequence new_out = parlay::tabulate(len, [&] (size_t j){
				return sorted[j].first;
			});
			v[i]->out_nbh = new_out;
		});
	}

	parlay::sequence<std::pair<int, parlay::sequence<int>>> reverse_graph_edges(parlay::sequence<tvec_point*> &v){
		// parlay::sequence<std::pair<int, parlay::sequence<int>>> reverse_graph(v.size());
		parlay::sequence<parlay::sequence<edge>> grouped_edges(v.size());
		parlay::parallel_for(0, v.size(), [&] (size_t i){
			parlay::sequence<edge> uedges = parlay::tabulate((v[i]->out_nbh).size(), [&] (size_t j) {
				return std::make_pair(v[i]->out_nbh[j], (int) i);});
			grouped_edges[i] = uedges;
		});
		auto edges = parlay::flatten(grouped_edges);
		auto reverse_graph = parlay::group_by_key(edges);
		return reverse_graph;
	}

	void undirect_graph(parlay::sequence<tvec_point*> &v){
		auto reverse_edges = reverse_graph_edges(v);
		parlay::parallel_for(0, reverse_edges.size(), [&] (size_t i){
			size_t index = reverse_edges[i].first;
			for(const int& j : reverse_edges[i].second) v[index]->out_nbh.push_back(j);
		});
	}

	void prune_triangles(parlay::sequence<tvec_point*> &v){
		parlay::parallel_for(0, v.size(), [&] (size_t i) {
			auto distances = parlay::tabulate(v[i]->out_nbh.size(), [&] (size_t j){
				float dist = distance(v[i]->coordinates.begin(), v[v[i]->out_nbh[j]]->coordinates.begin(), d);
				return std::make_pair(v[i]->out_nbh[j], dist);
			});
			auto less = [&] (pid a, pid b) {return a.second < b.second;};
			auto sorted = parlay::sort(distances);
			parlay::sequence<int> new_out = parlay::sequence<int>();
			for(const pid& j : sorted){
				if(new_out.size()==0) new_out.push_back(j.first);
				else{
					float dist_p = j.second;
					bool add = true;
					for(const int& k : new_out){
						float dist = distance(v[j.first]->coordinates.begin(), v[k]->coordinates.begin(), d);
						if(dist_p > dist) {add = false; break;}
					}
					if(add) new_out.push_back(j.first);
				}
			}
			v[i]->new_out_nbh = new_out;
		});
		parlay::parallel_for(0, v.size(), [&] (size_t i){
			v[i]->out_nbh = v[i]->new_out_nbh;
			v[i]->new_out_nbh.clear();
		});
	}


	void build_index(parlay::sequence<tvec_point*> &v, size_t cluster_size){
		clear(v);
		cluster<T> C(d);
		C.multiple_clustertrees(v, cluster_size, 20, &naive_neighbors, d, K);
		std::cout << "finished clustering" << std::endl; 
		truncate_graph(v, K);
		nn_descent_wrapper(v);
		// check_nearest_neighbors(v);
		undirect_graph(v);
		prune_triangles(v);
		truncate_graph(v, (int) 2*K);
	}

};

