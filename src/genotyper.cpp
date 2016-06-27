#include "genotyper.hpp"
#include "bubbles.hpp"
#include "distributions.hpp"

namespace vg {

using namespace std;

    // genotyper:
    // use graph and reads to:
    // - Augment graph
    // - Find superbubbles or cactus branches to determine sites
    // - Generate proposals for paths through each site (from reads?)
    // - Compute affinities of each read for each proposed path through a site
    // - Compute diploid genotypes for each site
    // - Output as vcf or as native format

void Genotyper::run(VG& graph,
                    vector<Alignment>& alignments,
                    ostream& out,
                    string ref_path_name,
                    string contig_name,
                    string sample_name,
                    string augmented_file_name,
                    bool use_cactus,
                    bool show_progress,
                    bool output_vcf,
                    bool output_json,
                    int length_override,
                    int variant_offset) {

    if(ref_path_name.empty()) {
        // Guess the ref path name
        if(graph.paths.size() == 1) {
            // Autodetect the reference path name as the name of the only path
            ref_path_name = (*graph.paths._paths.begin()).first;
        } else {
            ref_path_name = "ref";
        }
    }
    
    if(output_vcf && show_progress) {
        cerr << "Calling against path " << ref_path_name << endl;
    }
    
    if(sample_name.empty()) {
        // Set a default sample name
        sample_name = "SAMPLE";
    }

    // Make sure they have names. TODO: we assume that if they do have names
    // they are already unique, and that they aren't like the names we generate.
    for(size_t i = 0; i < alignments.size(); i++) {
        if(alignments[i].name().empty()) {
            // Generate a unique name
            alignments[i].set_name("_unnamed_alignment_" + to_string(i));
        }
    }
    
    // Suck out paths
    vector<Path> paths;
    for(auto& alignment : alignments) {
        // Copy over each path, naming it after its alignment
        Path path = alignment.path();
        path.set_name(alignment.name());
        paths.push_back(path);
    }
    
    // Run them through vg::edit() to add them to the graph. Save the translations.
    vector<Translation> augmentation_translations = graph.edit(paths);
    
    if(show_progress) {
        cerr << "Augmented graph; got " << augmentation_translations.size() << " translations" << endl;
    }

    // Set up the translator to map back
    Translator translator(augmentation_translations);
    
    // Make sure that we actually have an index for traversing along paths.
    graph.paths.rebuild_mapping_aux();
    
    if(!augmented_file_name.empty()) {
        ofstream augmented_stream(augmented_file_name);
        graph.serialize_to_ostream(augmented_stream);
        augmented_stream.close();
    }
    
    // store the reads that are embedded in the augmented graph, by their unique names
    map<string, Alignment*> reads_by_name;
    for(auto& alignment : alignments) {
        reads_by_name[alignment.name()] = &alignment;
        // Make sure to replace the alignment's path with the path it has in the augmented graph
        list<Mapping>& mappings = graph.paths.get_path(alignment.name());
        alignment.mutable_path()->clear_mapping();
        for(auto& mapping : mappings) {
            // Copy over all the transformed mappings
            *alignment.mutable_path()->add_mapping() = mapping;
        }
    }
    cerr << "Converted " << alignments.size() << " alignments to embedded paths" << endl;
    
    // Note that in os_x, iostream ends up pulling in headers that define a ::id_t type.
    
    // Unfold/unroll, find the superbubbles, and translate back.
    vector<Genotyper::Site> sites = use_cactus ? find_sites_with_cactus(graph, ref_path_name)
        : find_sites_with_supbub(graph);
    
    if(show_progress) {
        cerr << "Found " << sites.size() << " superbubbles" << endl;
    }
    
    // We're going to count up all the affinities we compute
    size_t total_affinities = 0;
    
    // We need a buffer for output
    vector<vector<Locus>> buffer;
    int thread_count = get_thread_count();
    buffer.resize(thread_count);
    
    // If we're doing VCF output we need a VCF header
    vcflib::VariantCallFile* vcf = nullptr;
    // And a reference index tracking the primary path
    ReferenceIndex* reference_index = nullptr;
    if(output_vcf) {
        // Build a reference index on our reference path
        reference_index = new ReferenceIndex(graph, ref_path_name);
        
        // Start up a VCF
        vcf = start_vcf(cout, *reference_index, sample_name, contig_name, length_override);
    }
        
    // We want to do this in parallel, but we can't #pragma omp parallel for over a std::map
    #pragma omp parallel shared(total_affinities)
    {
        #pragma omp single nowait
        {
            for(auto it = sites.begin(); it != sites.end(); it++) {
                // For each site in parallel
                
                #pragma omp task firstprivate(it) shared(total_affinities)
                {
                
                    auto& site = *it;
                    
                    int tid = omp_get_thread_num();
                    
                    // Get all the paths through the site
                    vector<list<NodeTraversal>> paths = get_paths_through_site(graph, site);
                    
                    if(paths.size() == 0) {
                        // TODO: this compensates for inside-out sites from
                        // Cactus. Make Cactus give us sites that can actually
                        // be traversed through.
                        
                        // Flip the site around and try again
                        swap(site.start, site.end);
                        vector<list<NodeTraversal>> reverse_paths = get_paths_through_site(graph, site);
                        if(reverse_paths.size() != 0) {
                            // We actually got some paths. Use them
                            swap(paths, reverse_paths);
                            #pragma omp critical (cerr)
                            cerr << "Warning! Corrected inside-out site " << site.end << " - " << site.start << endl;
                        } else {
                            // Put original start and end back for complaining
                            swap(site.start, site.end);
                        }
                    }
                    
                    if(reference_index != nullptr &&
                        reference_index->byId.count(site.start.node->id()) && 
                        reference_index->byId.count(site.end.node->id())) {
                        // This site is on the reference (and we are indexing a reference because we are going to vcf)
                        
                        // Where do the start and end nodes fall in the reference?
                        auto start_ref_appearance = reference_index->byId.at(site.start.node->id());
                        auto end_ref_appearance = reference_index->byId.at(site.end.node->id());
                        
                        
                        // Are the ends running with the reference (false) or against it (true)
                        auto start_rel_orientation = (site.start.backward != start_ref_appearance.second);
                        auto end_rel_orientation = (site.end.backward != end_ref_appearance.second);
                        
                        if(show_progress) {
                            // Determine where the site starts and ends along the reference path
                            #pragma omp critical (cerr)
                            cerr << "Site " << site.start << " - " << site.end << " runs reference " << 
                                start_ref_appearance.first << " to " << 
                                end_ref_appearance.first << endl;
                                
                            if(!start_rel_orientation && !end_rel_orientation &&
                                end_ref_appearance.first < start_ref_appearance.first) {
                                // The site runs backward in the reference (but somewhat sensibly).
                                #pragma omp critical (cerr)
                                cerr << "Warning! Site runs backwards!" << endl;
                            } 
                                
                        }
                            
                    }
                    
                    // Even if it looks like there's only one path, it might not
                    // be the reference path, because the reference path might
                    // not have passed the min recurrence filter. So we can't
                    // skip things yet.
                    if(paths.empty()) {
                        // Don't do anything for superbubbles with no routes through
                        if(show_progress) {
                            #pragma omp critical (cerr)
                            cerr << "Site " << site.start << " - " << site.end << " has " << paths.size() <<
                                " alleles: skipped for having no alleles" << endl;
                        }
                    } else {
                    
                        if(show_progress) {
                            #pragma omp critical (cerr)
                            cerr << "Site " << site.start << " - " << site.end << " has " << paths.size() << " alleles" << endl;
                            for(auto& path : paths) {
                                // Announce each allele in turn
                                #pragma omp critical (cerr)
                                cerr << "\t" << traversals_to_string(path) << endl;
                            }
                        }
                        
                        // Get the affinities for all the paths
                        map<Alignment*, vector<Genotyper::Affinity>> affinities = get_affinities_fast(graph, reads_by_name, site, paths);
                        
                        for(auto& alignment_and_affinities : affinities) {
                            #pragma omp critical (total_affinities)
                            total_affinities += alignment_and_affinities.second.size();
                        }
                        
                        // Get a genotyped locus in the original frame
                        Locus genotyped = genotype_site(graph, site, paths, affinities);
                        
                        if(output_json) {
                            // Dump in JSON
                            #pragma omp critical (cout)
                            cout << pb2json(genotyped) << endl;
                        } else if(output_vcf) {
                            // Get 0 or more variants from the superbubble
                            vector<vcflib::Variant> variants =
                                locus_to_variant(graph, site, *reference_index, *vcf, genotyped, sample_name);
                            for(auto& variant : variants) {
                                // Fix up all the variants
                                if(!contig_name.empty()) {
                                    // Override path name
                                    variant.sequenceName = contig_name;
                                } else {
                                    // Keep path name
                                    variant.sequenceName = ref_path_name;
                                }
                                variant.position += variant_offset;
                                #pragma omp critical(cout)
                                cout << variant << endl;
                            }
                        } else {
                            // Write out in Protobuf
                            buffer[tid].push_back(genotyped);
                            stream::write_buffered(cout, buffer[tid], 100);
                        }
                    }
                }
            }
        }
    }           
    
    if(!output_json && !output_vcf) {
        // Flush the protobuf output buffers
        for(int i = 0; i < buffer.size(); i++) {
            stream::write_buffered(cout, buffer[i], 0);
        }
    } 


    if(show_progress) {
        cerr << "Computed " << total_affinities << " affinities" << endl;
    }

}

/**
 * Turn the given path (which must be a thread) into an allele. Drops the first
 * and last mappings and looks up the sequences for the nodes of the others.
 */
string allele_to_string(VG& graph, const Path& allele) {
    stringstream stream;
    
    for(size_t i = 1; i < allele.mapping_size() - 1; i++) {
        // Get the sequence for each node
        string node_string = graph.get_node(allele.mapping(i).position().node_id())->sequence();
        
        if(allele.mapping(i).position().is_reverse()) {
            // Flip it
            node_string = reverse_complement(node_string);
        }
        // Add it to the stream
        stream << node_string;
    }
    
    return stream.str();
}

int Genotyper::alignment_qual_score(const Alignment& alignment) {
    if(alignment.quality().empty()) {
        // Special case: qualities not given. Assume something vaguely sane so
        // we can genotype without quality.
#ifdef debug
        cerr << "No base qualities. Assuming default quality of " << default_sequence_quality << endl;
#endif
        return default_sequence_quality;
    }
    
    double total = 0;
    for(size_t i = 0; i < alignment.quality().size(); i++) {
#ifdef debug
        cerr << "Quality: " << (int)alignment.quality()[i] << endl;
#endif
        total += alignment.quality()[i];
    }
#ifdef debug
    cerr << "Count: " << alignment.quality().size() << endl;
#endif
    // Make the total now actually be an average
    total /= alignment.quality().size();
    return round(total);
}

vector<Genotyper::Site> Genotyper::find_sites_with_supbub(VG& graph) {

    // Set up our output vector
    vector<Site> to_return;

    // Unfold the graph
    // Copy the graph and unfold the copy. We need to hold this translation
    // table from new node ID to old node and relative orientation.
    map<vg::id_t, pair<vg::id_t, bool>> unfold_translation;
    auto transformed = graph.unfold(unfold_max_length, unfold_translation);
    
    // Fix up any doubly reversed edges
    transformed.flip_doubly_reversed_edges();

    // Now dagify the graph. We need to hold this translation table from new
    // node ID to old node and relative orientation.
    map<vg::id_t, pair<vg::id_t, bool>> dag_translation;
    transformed = transformed.dagify(dagify_steps, dag_translation);
    
    // Compose the complete translation
    map<vg::id_t, pair<vg::id_t, bool>> overall_translation = transformed.overlay_node_translations(dag_translation, unfold_translation);
    dag_translation.clear();
    unfold_translation.clear();
    
    // Find the superbubbles in the DAG
    map<pair<id_t, id_t>, vector<id_t>> superbubbles = vg::superbubbles(transformed);
    
    for(auto& superbubble : superbubbles) {
        
        // Translate the superbubble coordinates into NodeTraversals.
        // This is coming from a DAG so we only need to look at the translation for orientation.
        auto& start_translation = overall_translation[superbubble.first.first];
        NodeTraversal start(graph.get_node(start_translation.first), start_translation.second);
        
        auto& end_translation = overall_translation[superbubble.first.second];
        NodeTraversal end(graph.get_node(end_translation.first), end_translation.second);
        
        // Make a Site and tell it where to start and end
        Site site;
        site.start = start;
        site.end = end;

        for(auto id : superbubble.second) {
            // Translate each ID and put it in the set
            site.contents.insert(overall_translation[id].first);
        }
        
        // Save the site
        to_return.emplace_back(std::move(site));
    }

    // Give back the collection of sites
    return to_return;    
    
}

vector<Genotyper::Site> Genotyper::find_sites_with_cactus(VG& graph, const string& ref_path_name) {

    // Set up our output vector
    vector<Site> to_return;

    // cactus needs the nodes to be sorted in order to find a source and sink
    graph.sort();
    
    // get endpoints using node ranks
    pair<id_t, id_t> source_sink = get_cactus_source_sink(graph, ref_path_name);

    // todo: use deomposition instead of converting tree into flat structure
    BubbleTree bubble_tree = cactusbubble_tree(graph, source_sink);

    // copy nodes up to bubbles that contain their bubble
    bubble_up_bubbles(bubble_tree);

    bubble_tree.for_each_preorder([&](BubbleTree::Node* node) {
            Bubble& bubble = node->v;
            // cut root to be consistent with superbubbles()
            if (bubble.start != bubble_tree.root->v.start ||
                bubble.end != bubble_tree.root->v.end) {
                set<id_t> nodes{bubble.contents.begin(), bubble.contents.end()};
                NodeTraversal start(graph.get_node(bubble.start.node), bubble.start.is_end);
                NodeTraversal end(graph.get_node(bubble.end.node), bubble.end.is_end);
                // Fill in a Site. Make sure to preserve original endpoint
                // ordering, because swapping them without flipping their
                // orientation flags will make an inside-out site.
                Site site;
                site.start = start;
                site.end = end;
                swap(site.contents, nodes);
                // Save the site
                to_return.emplace_back(std::move(site));
            }
        });    

    return to_return;
}

vector<list<NodeTraversal>> Genotyper::get_paths_through_site(VG& graph, const Site& site) {
    // We're going to emit traversals supported by any paths in the graph.
    
    // Put all our subpaths in here to deduplicate them by sequence they spell out. And to count occurrences.
    map<string, pair<list<NodeTraversal>, int>> results;

#ifdef debug
    cerr << "Looking for paths between " << site.start << " and " << site.end << endl;
#endif
    
    if(graph.paths.has_node_mapping(site.start.node) && graph.paths.has_node_mapping(site.end.node)) {
        // If we have some paths that visit both ends (in some orientation)
        
        // Get all the mappings to the end node, by path name
        auto& endmappings_by_name = graph.paths.get_node_mapping(site.end.node);
        
        for(auto& name_and_mappings : graph.paths.get_node_mapping(site.start.node)) {
            // Go through the paths that visit the start node
            
            if(!endmappings_by_name.count(name_and_mappings.first)) {
                //cerr << "no endmappings match" << endl;
                // No path by this name has any mappings to the end node. Skip
                // it early.
                continue;
            }
            
            for(auto* mapping : name_and_mappings.second) {
                // Start at each mapping in the appropriate orientation
                
#ifdef debug
                cerr << "Trying mapping of read " << name_and_mappings.first << endl;
#endif
                
                // How many times have we gone to the next mapping looking for a
                // mapping to the end node in the right orientation?
                size_t traversal_count = 0;
                
                // Do we want to go left (true) or right (false) from this
                // mapping? If start is a forward traversal and we found a
                // forward mapping, we go right. If either is backward we go
                // left, and if both are backward we go right again.
                bool traversal_direction = mapping->position().is_reverse() != site.start.backward;
                
                // What orientation would we want to find the end node in? If
                // we're traveling backward, we expect to find it in the
                // opposite direction to the one we were given.
                bool expected_end_orientation = site.end.backward != traversal_direction;
                
                // We're going to fill in this list with traversals.
                list<NodeTraversal> path_traversed;
                
                // And we're going to fill this with the sequence
                stringstream allele_stream;
                
                while(mapping != nullptr && traversal_count < max_path_search_steps) {
                    // Traverse along until we hit the end traversal or take too
                    // many steps
                    
#ifdef debug
                    cerr << "\tTraversing " << pb2json(*mapping) << endl;
#endif
                    
                    // Say we visit this node along the path, in this orientation
                    path_traversed.push_back(NodeTraversal(graph.get_node(mapping->position().node_id()), mapping->position().is_reverse() != traversal_direction));
                    
                    // Stick the sequence of the node (appropriately oriented) in the stream for the allele sequence
                    string seq = graph.get_node(mapping->position().node_id())->sequence();
                    allele_stream << (path_traversed.back().backward ? reverse_complement(seq) : seq);
                    
                    if(mapping->position().node_id() == site.end.node->id() && mapping->position().is_reverse() == expected_end_orientation) {
                        // We have stumbled upon the end node in the orientation we wanted it in.
                        if(results.count(allele_stream.str())) {
                            // It is already there! Increment the observation count.
#ifdef debug
                            cerr << "\tFinished; got known sequence " << allele_stream.str() << endl;
#endif
                            results[allele_stream.str()].second++;
                        } else {
                            // Add it in
                            results[allele_stream.str()] = make_pair(path_traversed, 1);
#ifdef debug
                            cerr << "\tFinished; got novel sequence " << allele_stream.str() << endl;
#endif
                        }
                        
                        // Then try the next embedded path
                        break;
                    }
                    
                    // Otherwise just move to the right (or left)
                    if(traversal_direction) {
                        // We're going backwards
                        mapping = graph.paths.traverse_left(mapping);
                    } else {
                        // We're going forwards
                        mapping = graph.paths.traverse_right(mapping);
                    }
                    // Tick the counter so we don't go really far on long paths.
                    traversal_count++;
                    
                }
                
                
            }
        }
        
    }
    
    // Now collect the unique results
    vector<list<NodeTraversal>> to_return;
    
    for(auto& result : results) {
        // Break out each result
        const string& seq = result.first;
        auto& traversals = result.second.first;
        auto& count = result.second.second;
        
        if(count < min_recurrence) {
            // We don't have enough initial hits for this sequence to justify
            // trying to re-align the rest of the reads. Skip it. Note that the
            // reference path counts as a single recurrence, so we might throw
            // it out if nothing else covers it.
            continue;
        }
        
        // Send out each list of traversals
        to_return.emplace_back(std::move(traversals));
    }
    
    return to_return;
}

map<Alignment*, vector<Genotyper::Affinity>> Genotyper::get_affinities(VG& graph, const map<string, Alignment*>& reads_by_name,
    const Site& site, const vector<list<NodeTraversal>>& superbubble_paths) {
    
    // Grab our thread ID, which determines which aligner we get.
    int tid = omp_get_thread_num();
    
    // We're going to build this up gradually, appending to all the vectors.
    map<Alignment*, vector<Affinity>> to_return;
    
    // What reads are relevant to this superbubble?
    set<string> relevant_read_names;
    
#ifdef debug
    cerr << "Superbubble contains " << site.contents.size() << " nodes" << endl;
#endif

    for(auto id : site.contents) {
        // For every node in the superbubble, what paths visit it?
        auto& mappings_by_name = graph.paths.get_node_mapping(id);
        for(auto& name_and_mappings : mappings_by_name) {
            // For each path visiting the node
            if(reads_by_name.count(name_and_mappings.first)) {
                // This path is a read, so add the name to the set if it's not
                // there already
                relevant_read_names.insert(name_and_mappings.first);
            }    
        }
    }
    
    // What IDs are visited by these reads?
    set<id_t> relevant_ids;
    
    for(auto& name : relevant_read_names) {
        // Get the mappings for each read
        auto& mappings = graph.paths.get_path(name);
        for(auto& mapping : mappings) {
            // Add in all the nodes that are visited
            relevant_ids.insert(mapping.position().node_id());
        }
    }
    
    for(auto id : site.contents) {
        // Throw out all the IDs that are also used in the superbubble itself
        relevant_ids.erase(id);
    }
    
#ifdef debug
    cerr << relevant_read_names.size() << " reads visit an additional " << relevant_ids.size() << " nodes" << endl;
#endif
    
    // Make a vg graph with all the nodes used by the reads relevant to the
    // superbubble, but outside the superbubble itself.
    VG surrounding;
    for(auto id : relevant_ids) {
        // Add each node and its edges to the new graph. Ignore dangling edges.
        // We'll keep edges dangling to the superbubble anchor nodes.
        surrounding.add_node(*graph.get_node(id));
        surrounding.add_edges(graph.edges_of(graph.get_node(id)));
    }
    
    for(auto& path : superbubble_paths) {
        // Now for each superbubble path, make a copy of that graph with it in
        VG allele_graph(surrounding);
        
        for(auto it = path.begin(); it != path.end(); ++it) {
            // Add in every node on the path to the new allele graph
            allele_graph.add_node(*(*it).node);
            
            // Add in just the edge to the previous node on the path
            if(it != path.begin()) {
                // There is something previous on the path.
                auto prev = it;
                --prev;
                // Make an edge
                Edge path_edge;
                // And hook it to the correct side of the last node
                path_edge.set_from((*prev).node->id());
                path_edge.set_from_start((*prev).backward);
                // And the correct side of the next node
                path_edge.set_to((*it).node->id());
                path_edge.set_to_end((*it).backward);
                
                assert(graph.has_edge(path_edge));
                
                // And add it in
                allele_graph.add_edge(path_edge);
            }
        }
        
        // Get rid of dangling edges
        allele_graph.remove_orphan_edges();
        
#ifdef debug
        #pragma omp critical (cerr)
        cerr << "Align to " << pb2json(allele_graph.graph) << endl;
#endif
        
        for(auto& name : relevant_read_names) {
            // For every read that touched the superbubble, grab its original
            // Alignment pointer.
            Alignment* read = reads_by_name.at(name);
            
            // Look to make sure it touches more than one node actually in the
            // superbubble, or a non-start, non-end node. If it just touches the
            // start or just touches the end, it can't be informative.
            set<id_t> touched_set;
            // Will this read be informative?
            bool informative = false;            
            for(size_t i = 0; i < read->path().mapping_size(); i++) {
                // Look at every node the read touches
                id_t touched = read->path().mapping(i).position().node_id();
                if(site.contents.count(touched)) {
                    // If it's in the superbubble, keep it
                    touched_set.insert(touched);
                }
            }
            
            if(touched_set.size() >= 2) {
                // We touch both the start and end, or an internal node.
                informative = true;
            } else {
                // Throw out the start and end nodes, if we touched them.
                touched_set.erase(site.start.node->id());
                touched_set.erase(site.end.node->id());
                if(!touched_set.empty()) {
                    // We touch an internal node
                    informative = true;
                }
            }
            
            if(!informative) {
                // We only touch one of the start and end nodes, and can say nothing about the superbubble. Try the next read.
                continue;
            }
            
            // If we get here, we know this read is informative as to the internal status of this superbubble.
            Alignment aligned_fwd;
            Alignment aligned_rev;
            // We need a way to get graph node sizes to reverse these alignments
            auto get_node_size = [&](id_t id) {
                return graph.get_node(id)->sequence().size();
            };
            if(read->sequence().size() == read->quality().size()) {
                // Re-align a copy to this graph (using quality-adjusted alignment).
                // TODO: actually use quality-adjusted alignment
                aligned_fwd = allele_graph.align(*read);
                aligned_rev = allele_graph.align(reverse_complement_alignment(*read, get_node_size));
            } else {
                // If we don't have the right number of quality scores, use un-adjusted alignment instead.
                aligned_fwd = allele_graph.align(*read);
                aligned_rev = allele_graph.align(reverse_complement_alignment(*read, get_node_size));
            }
            // Pick the best alignment, and emiot in original orientation
            Alignment aligned = (aligned_rev.score() > aligned_fwd.score()) ? reverse_complement_alignment(aligned_rev, get_node_size) : aligned_fwd;
            
#ifdef debug
            #pragma omp critical (cerr)
            cerr << "\t" << pb2json(aligned) << endl;
#endif

            // Grab the identity and save it for this read and superbubble path
            to_return[read].push_back(Affinity(aligned.identity(), aligned_rev.score() > aligned_fwd.score()));
            
        }
    }
    
    // After scoring all the reads against all the versions of the superbubble,
    // return the affinities
    return to_return;
}


list<NodeTraversal> Genotyper::get_traversal_of_site(VG& graph, const Site& site, const Path& path) {
    
    // We'll fill this in
    list<NodeTraversal> to_return;
    
    for(size_t i = 0; i < path.mapping_size(); i++) {
        // Make a NodeTraversal version of the Mapping
        NodeTraversal traversal(graph.get_node(path.mapping(i).position().node_id()), path.mapping(i).position().is_reverse());
        
        if(site.contents.count(traversal.node->id())) {
            // We're inside the bubble. This is super simple when we have the contents!
            to_return.push_back(traversal);
        }
    
    }
    return to_return;
}

string Genotyper::traversals_to_string(const list<NodeTraversal>& path) {
    stringstream seq;
    for(auto& traversal : path) {
        // Stick in each sequence in order, with orientation
        seq << (traversal.backward ? reverse_complement(traversal.node->sequence()) : traversal.node->sequence());
    }
    return seq.str();
}

map<Alignment*, vector<Genotyper::Affinity>> Genotyper::get_affinities_fast(VG& graph, const map<string, Alignment*>& reads_by_name,
    const Site& site, const vector<list<NodeTraversal>>& superbubble_paths) {
    
    // Grab our thread ID, which determines which aligner we get.
    int tid = omp_get_thread_num();
    
    // We're going to build this up gradually, appending to all the vectors.
    map<Alignment*, vector<Affinity>> to_return;
    
    // What reads are relevant to this superbubble?
    set<string> relevant_read_names;
    
#ifdef debug
    cerr << "Superbubble contains " << site.contents.size() << " nodes" << endl;
#endif

    // Convert all the Paths used for alleles back to their strings.
    vector<string> allele_strings;
    for(auto& path : superbubble_paths) {
        // Convert all the Paths used for alleles back to their strings.
        allele_strings.push_back(traversals_to_string(path));
    }
    
    for(auto id : site.contents) {
        // For every node in the superbubble, what paths visit it?
        auto& mappings_by_name = graph.paths.get_node_mapping(id);
        for(auto& name_and_mappings : mappings_by_name) {
            // For each path visiting the node
            if(reads_by_name.count(name_and_mappings.first)) {
                // This path is a read, so add the name to the set if it's not
                // there already
                relevant_read_names.insert(name_and_mappings.first);
            }    
        }
    }
    
    for(auto name : relevant_read_names) {
        // For each relevant read, work out a string for the superbubble and whether
        // it's anchored on each end.
        
        // Make an Affinity to fill in
        Affinity base_affinity;
        
        // Get the NodeTraversals for this read through this site.
        auto read_traversal = get_traversal_of_site(graph, site, reads_by_name.at(name)->path());
        
        if(read_traversal.front() == site.end.reverse() || read_traversal.back() == site.start.reverse()) {
            // We really traversed this site backward. Flip it around.
            read_traversal.reverse();
            for(auto& item : read_traversal) {
                // Flip around every traversal as well as reversing their order.
                item = item.reverse();
            }
            
            // We're on the reverse strand
            base_affinity.is_reverse = true;
        }
        
        size_t total_supported = 0;
        
        // Get the string it spells out
        auto seq = traversals_to_string(read_traversal);
        
        // Now decide if the read's seq supports each path.
        for(auto& path_seq : allele_strings) {
            // We'll make an affinity for this allele
            Affinity affinity = base_affinity;
            if(read_traversal.front() == site.start && read_traversal.back() == site.end) {
                // Anchored at both ends.
                // Need an exact match. Record if we have one or not.
                affinity.consistent = (seq == path_seq);
            } else if(read_traversal.front() == site.start) {
                // Anchored at start only.
                // seq needs to be a prefix of path_seq
                auto difference = std::mismatch(seq.begin(), seq.end(), path_seq.begin());
                // If the first difference is the past-the-end of the prefix, then it's a prefix
                affinity.consistent = (difference.first == seq.end());
            } else if(read_traversal.back() == site.end) {
                // Anchored at end only.
                // seq needs to be a suffix of path_seq
                auto difference = std::mismatch(seq.rbegin(), seq.rend(), path_seq.rbegin());
                // If the first difference is the past-the-rend of the suffix, then it's a suffix
                affinity.consistent = (difference.first == seq.rend());
            } else {
                // This read doesn't touch either end.
                cerr << "Warning: read doesn't touch either end of its site!" << endl;
            }
            
            // Fake a weight
            affinity.affinity = (double)affinity.consistent;
            to_return[reads_by_name.at(name)].push_back(affinity);
            
            // Add in to the total if it supports this
            total_supported += affinity.consistent;
        }
        
        if(total_supported == 0 && min_recurrence <= 1) {
            // This is weird. Doesn't match anything and we had no excuse to remove alleles.
            cerr << "Warning! Bubble sequence " << seq << " supports nothing!" << endl;
        }
        
        
    }
    
    
    // After scoring all the reads against all the versions of the superbubble,
    // return the affinities
    return to_return;
}

double Genotyper::get_genotype_log_likelihood(const vector<int>& genotype, const vector<pair<Alignment, vector<Affinity>>> alignment_consistency) {
    // For each genotype, calculate P(observed reads | genotype) as P(all reads
    // that don't support an allele from the genotype are mismapped or
    // miscalled) * P(all reads that do support alleles from the genotype ended
    // up apportioned across the alleles as they are)
    
    // This works out to the product over all reads that don't support either
    // alleles of 1 - ((1 - MAPQ) * (1 - P(bases called wrong)), times the
    // product over all the reads that do support one of the alleles of P(that
    // allele picked out of the one or two available).
    
    // TODO: handle contamination like Freebayes
    
    // This is the log probability that all reads that don't support either allele in this genotype are wrong.
    double all_non_supporting_wrong = prob_to_logprob(1);
    
    // This is the log probability that all the reads that do support alleles in this genotype were drawn from the genotype they support.
    double all_supporting_drawn = prob_to_logprob(1);
    
#ifdef debug
    if(genotype.size() > 1) {
        cerr << "Calculating P(a" << genotype[0] << "/a" << genotype[1] << ")" << endl;
    }
#endif

    // We want to keep count of how many reads support each allele in each
    // orientation. TODO: we might be sort of double-counting reads that support
    // multiple alleles, because we'll be treating them as having independent
    // orientations per allele, when really they're very likely to be oriented
    // the same way.
    
    // Maps from int allele number (as appears in Genotype) to total reads
    // forward and reverse giving support.
    map<int, pair<int, int>> strand_count_by_allele_and_orientation;
    
    for(auto& read_and_consistency : alignment_consistency) {
        // For each read, work out if it supports a genotype we have or not.
        
        // Split out the alignment from its consistency flags
        auto& read = read_and_consistency.first;
        auto& consistency = read_and_consistency.second;
        
        // How many of the alleles in our genotype is it consistent with?
        int consistent_alleles = 0;
        for(int allele : genotype) {
            if(consistency.at(allele).consistent) {
                // We're consistent with this allele
                consistent_alleles++;
                // And in this orientation
                if(consistency.at(allele).is_reverse) {
                    // Consistent with reverse
                    strand_count_by_allele_and_orientation[allele].second++;
                } else {
                    // Consistent with forward
                    strand_count_by_allele_and_orientation[allele].first++;
                }
            }
        }
        
        auto read_qual = alignment_qual_score(read);
        
#ifdef debug
        cerr << "Read (qual score " << read_qual << ") consistent with " << consistent_alleles
            << " genotype alleles observed." << endl;
#endif
        
        if(consistent_alleles == 0) {
            // This read is inconsistent with all the alleles in the genotype,
            // so, given the genotype, the read must be sequenced or mapped
            // wrong.
            
            double logprob_wrong;
            if(use_mapq) {
                // Compute P(mapped wrong or called wrong) = P(not (mapped right and called right)) = P(not (not mapped wrong and not called wrong))
                logprob_wrong = logprob_invert(logprob_invert(phred_to_logprob(read.mapping_quality())) +
                    logprob_invert(phred_to_logprob(read_qual)));
            } else {
                // Compute P(called wrong).
                logprob_wrong = phred_to_logprob(read_qual);
            }
#ifdef debug
            cerr << "P(wrong) = " << logprob_to_prob(logprob_wrong) << endl;
#endif
            all_non_supporting_wrong += logprob_wrong;
        } else {
            // This read is consistent with some of the alleles in the genotype,
            // so we must have drawn one of those alleles when sequencing.
            
            // We account for this in a framework where we consider the reads
            // indistinguishable, so ignore it for now.
            
            // So multiply in the probability that we hit one of those alleles
            double logprob_drawn = prob_to_logprob(((double) consistent_alleles) / genotype.size());
            
#ifdef debug
            cerr << "P(drawn) = " << logprob_to_prob(logprob_drawn) << endl;
#endif
            
            all_supporting_drawn += logprob_drawn;
        }
        
    }
    
    // Multiply in in the probability that the supporting reads all came from
    // the strands they are on.
    double strands_as_specified = prob_to_logprob(1);
    // Each strand is equally likely
    vector<double> probs_by_orientation = {0.5, 0.5};
    for(auto& kv : strand_count_by_allele_and_orientation) {
        // For the forward and reverse strand counts for all the alleles
        auto& forward_count = kv.second.first;
        auto& reverse_count = kv.second.second;
        
#ifdef debug
            cerr << "Allele "  << kv.first << " supported by " << forward_count << " forward, "
                << reverse_count << " reverse" << endl;
#endif

        // Convert to a vector to satisfy the multinomial PMF function.
        vector<int> obs = {forward_count, reverse_count};

        // Get the log probability under multinomial.
        double logprob = multinomial_sampling_prob_ln(probs_by_orientation, obs);
        
#ifdef debug
            cerr << "P = " << logprob_to_prob(logprob) << endl;
#endif

        strands_as_specified += logprob;
        
    }

    // TODO: handle support counts as if reads are indistinguishable as well.
    // This gets complicated because of multi-support; we can't just use the
    // multinomial PMF.
    
    // Now we've looked at all the reads, so AND everything together
    return all_non_supporting_wrong + all_supporting_drawn + strands_as_specified;
}

double Genotyper::get_genotype_log_prior(const vector<int>& genotype) {
    assert(genotype.size() == 2);
    
    
    // Priors are boring: certain amount for het, inverse of that for everyone else
    if(genotype[0] != genotype[1]) {
        // This is a het!
        return het_prior_logprob;
    } else {
        // This is a homozygote. Much more common.
        return logprob_invert(het_prior_logprob);
    }
}

string Genotyper::get_qualities_in_site(VG& graph, const Site& site, const Alignment& alignment) {
    // We'll fill this in.
    stringstream to_return;
    
    // Are we currently in the site?
    bool in_site = false;
    // What NodeTraversal do we need to see to leave?
    NodeTraversal expected;
    
    // Where are we in the quality string?
    size_t quality_pos = 0;
    
    for(size_t i = 0; i < alignment.path().mapping_size(); i++) {
        // For every mapping in the path in order
        auto& mapping = alignment.path().mapping(i);
        
        // What NodeTraversal is this?
        NodeTraversal traversal(graph.get_node(mapping.position().node_id()), mapping.position().is_reverse());
        
        if(!in_site) {
            // If we aren't in the site, we may be entering
            if(traversal == site.start) {
                // We entered through the start
                in_site = true;
                // We'll leave at the end
                expected = site.end;
            } else if(traversal == site.end.reverse()) {
                // We entered through the end
                in_site = true;
                // We'll leave when we hit the start in reverse
                expected = site.start.reverse();
            }
        }
        
        for(size_t j = 0; j < mapping.edit_size(); j++) {
            // For every edit
            auto& edit = mapping.edit(j);
        
            if(in_site) {
                for(size_t k = 0; k < edit.to_length(); k++) {
                    // Take each quality value from the edit and add it to our collection to return
                    if(quality_pos >= alignment.quality().size()) {
                        // If we've run out of quality values, give back no
                        // qualities, because base qualities aren't really being
                        // used.
                        return "";
                    }
                    to_return << (char)alignment.quality().at(quality_pos);
                    quality_pos++;
                }
            } else {
                // Skip this edit's qualities 
                quality_pos += edit.to_length();
            }
        }
        
        if(in_site && traversal == expected) {
            // This was the node we were supposed to leave the site at.
            in_site = false;
        }
    }
    
    return to_return.str();
    
}

#define debug
Locus Genotyper::genotype_site(VG& graph, const Site& site, const vector<list<NodeTraversal>>& superbubble_paths, const map<Alignment*, vector<Affinity>>& affinities) {
    
    // Freebayes way (improved with multi-support)
    
    // We're going to populate this locus
    Locus to_return;
    
    for(auto& path : superbubble_paths) {
        // Convert each allele to a Path and put it in the locus
        *to_return.add_allele() = path_from_node_traversals(path);
    }
    
#ifdef debug
    cerr << "Looking between " << site.start << " and " << site.end << endl;
#endif
    
    // We'll fill this in with the trimmed alignments and their consistency-with-alleles flags.
    vector<pair<Alignment, vector<Affinity>>> alignment_consistency;
    
    // We fill this in with totals of reads supporting alleles
    vector<int> reads_consistent_with_allele(superbubble_paths.size(), 0);
    // And this with the same thing split out by forward and reverse strand
    vector<pair<int, int>> strand_support_for_allele(superbubble_paths.size(), make_pair(0, 0));
    
    // We'll store affinities by read name and allele here, for printing later.
    map<string, vector<Affinity>> debug_affinities;
    
    // We track overall forward and reverse strand reads, of reads that
    // support any allele.
    size_t overall_forward_reads = 0;
    size_t overall_reverse_reads = 0;
    
    for(auto& alignment_and_affinities : affinities) {
        // We need to clip down to just the important quality values        
        Alignment trimmed = *alignment_and_affinities.first;
        
        // Trim down qualities to just those in the superbubble
        auto trimmed_qualities = get_qualities_in_site(graph, site, trimmed);
        
        // Stick them back in the now bogus-ified Alignment object.
        trimmed.set_quality(trimmed_qualities);
        
        // Hide all the affinities where we can pull them later
        debug_affinities[trimmed.name()] = alignment_and_affinities.second;
        
        // Affinities already know whether they are consistent with an allele. Don't second-guess them.
        // Fine even with alignment; no read we embedded should ever have non-perfect identity.

        // We'll set these if the read supports anything in a forward or reverse
        // orientation.
        bool is_forward = false;
        bool is_reverse = false;
        
        for(size_t i = 0; i < alignment_and_affinities.second.size(); i++) {
            // Count up reads consistent with each allele
            if(alignment_and_affinities.second.at(i).consistent) {
                // This read is consistent with this allele
                reads_consistent_with_allele[i]++;
                if(alignment_and_affinities.second.at(i).is_reverse) {
                    // It is on the reverse strand
                    strand_support_for_allele[i].second++;
                    is_reverse = true;
                } else {
                    // It is on the forward strand
                    strand_support_for_allele[i].first++;
                    is_forward = true;
                }
            }
        }
        
        if(is_forward) {
            if(is_reverse) {
                // This is weird
                cerr << "Warning! Read supports alleles as both forward and reverse!" << endl;
                // Just call it forward
            }
            // This read supports an allele forward, so call it a forward read for the site
            overall_forward_reads++;
        } else if(is_reverse) {
            // This read supports an allele reverse, so call it a reverse read for the site
            overall_reverse_reads++;
        } else if(min_recurrence <= 1) {
            // Reads generally ought to support at least one allele, unless we
            // have weird softclips or they were for elided non-recurrent
            // alleles.
            cerr << "Warning! Read supports no alleles!" << endl;
        }
        
        // Save the trimmed alignment and its affinities, which we use to get GLs.
        alignment_consistency.emplace_back(std::move(trimmed), alignment_and_affinities.second);
        
    }
    
#ifdef debug
    for(int i = 0; i < reads_consistent_with_allele.size(); i++) {
        // Build a useful name for the allele
        stringstream allele_name;
        for(auto& traversal : superbubble_paths[i]) {
            allele_name << traversal.node->id() << ",";
        }
        cerr << "a" << i << "(" << allele_name.str() << "): " << reads_consistent_with_allele[i] << "/" << affinities.size() << " reads consistent" << endl;
        for(auto& read_and_consistency : alignment_consistency) {
            if(read_and_consistency.second[i].consistent && read_and_consistency.first.sequence().size() < 30) {
                // Dump all the short consistent reads
                cerr << "\t" << read_and_consistency.first.sequence() << " " << debug_affinities[read_and_consistency.first.name()][i].affinity << endl;
            }
        }
    }
#endif

    // We'll go through all the genotypes, fill in their probabilities, put them
    // in here, and then sort them to find the best.
    vector<Genotype> genotypes_sorted;

    for(int allele1 = 0; allele1 < superbubble_paths.size(); allele1++) {
        // For each first allele in the genotype
        for(int allele2 = 0; allele2 <= allele1; allele2++) {
            // For each second allele so we get all order-independent combinations
            
            // Make the combo
            vector<int> genotype_vector = {allele1, allele2};
            
            // Compute the log probability of the data given the genotype
            double log_likelihood = get_genotype_log_likelihood(genotype_vector, alignment_consistency);
            
            // Compute the prior
            double log_prior = get_genotype_log_prior(genotype_vector);
            
            // Apply Bayes Rule
            double log_posterior_unnormalized = log_likelihood + log_prior;
            
#ifdef debug
            cerr << "P(obs | a" << allele1 << "/a" << allele2 << ") = " << logprob_to_prob(log_likelihood) <<
                " (" << log_likelihood << ")" << endl;
            cerr << "P(a" << allele1 << "/a" << allele2 << ") = " << logprob_to_prob(log_prior) <<
                " (" << log_prior << ")" << endl;
            cerr << "P(a" << allele1 << "/a" << allele2 << " | obs) * P(obs) = " <<
                logprob_to_prob(log_posterior_unnormalized) << " (" << log_posterior_unnormalized << ")" << endl;
#endif

            // Fill in the actual Genotype object
            Genotype genotype;
            genotype.set_log_likelihood(log_likelihood);
            genotype.set_log_prior(log_prior);
            genotype.set_log_posterior(log_posterior_unnormalized);
            
            for(auto allele_id : genotype_vector) {
                // Copy over all the indexes of alleles in the genotype
                genotype.add_allele(allele_id);
            }
            
            // Put it in to sort
            genotypes_sorted.push_back(genotype);
        }
    }
    
    // Sort the genotypes in order of descending log posterior.
    sort(genotypes_sorted.begin(), genotypes_sorted.end(), [](const Genotype& a, const Genotype& b) {
        return a.log_posterior() > b.log_posterior();
    });
    
    for(size_t i = 0; i < superbubble_paths.size(); i++) {
        // For each allele, make a support
        Support* support = to_return.add_support();
        // Set forward and reverse depth
        support->set_forward(strand_support_for_allele[i].first);
        support->set_reverse(strand_support_for_allele[i].second);
    }
    
    for(auto& genotype : genotypes_sorted) {
        // Add a genotype to the Locus for every one we looked at, in order by descending posterior
        *to_return.add_genotype() = genotype;
    }
    
    // Set up total support for overall depth
    Support* overall_support = to_return.mutable_overall_support();
    overall_support->set_forward(overall_forward_reads);
    overall_support->set_reverse(overall_reverse_reads);
    
    // Now we've populated the genotype so return it.
    return to_return;
}
#undef debug

/**
 * Create the reference allele for an empty vcflib Variant, since apaprently
 * there's no method for that already. Must be called before any alt alleles are
 * added.
 */
void create_ref_allele(vcflib::Variant& variant, const std::string& allele) {
    // Set the ref allele
    variant.ref = allele;
    
    for(size_t i = 0; i < variant.ref.size(); i++) {
        // Look at all the bases
        if(variant.ref[i] != 'A' && variant.ref[i] != 'C' && variant.ref[i] != 'G' && variant.ref[i] != 'T') {
            // Correct anything bogus (like "X") to N
            variant.ref[i] = 'N';
        }
    }
    
    // Make it 0 in the alleles-by-index list
    variant.alleles.push_back(allele);
    // Build the reciprocal index-by-allele mapping
    variant.updateAlleleIndexes();
}

/**
 * Add a new alt allele to a vcflib Variant, since apaprently there's no method
 * for that already.
 *
 * If that allele already exists in the variant, does not add it again.
 *
 * Retuerns the allele number (0, 1, 2, etc.) corresponding to the given allele
 * string in the given variant. 
 */
int add_alt_allele(vcflib::Variant& variant, const std::string& allele) {
    // Copy the allele so we can throw out bad characters
    std::string fixed(allele);
    
    for(size_t i = 0; i < fixed.size(); i++) {
        // Look at all the bases
        if(fixed[i] != 'A' && fixed[i] != 'C' && fixed[i] != 'G' && fixed[i] != 'T') {
            // Correct anything bogus (like "X") to N
            fixed[i] = 'N';
        }
    }
    
    for(int i = 0; i < variant.alleles.size(); i++) {
        if(variant.alleles[i] == fixed) {
            // Already exists
            return i;
        }
    }

    // Add it as an alt
    variant.alt.push_back(fixed);
    // Make it next in the alleles-by-index list
    variant.alleles.push_back(fixed);
    // Build the reciprocal index-by-allele mapping
    variant.updateAlleleIndexes();

    // We added it in at the end
    return variant.alleles.size() - 1;
}

void Genotyper::write_vcf_header(std::ostream& stream, const std::string& sample_name, const std::string& contig_name, size_t contig_size) {
    stream << "##fileformat=VCFv4.2" << std::endl;
    stream << "##ALT=<ID=NON_REF,Description=\"Represents any possible alternative allele at this location\">" << std::endl;
    stream << "##INFO=<ID=XREF,Number=0,Type=Flag,Description=\"Present in original graph\">" << std::endl;
    stream << "##INFO=<ID=XSEE,Number=.,Type=String,Description=\"Original graph node:offset cross-references\">" << std::endl;
    stream << "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Total Depth\">" << std::endl;
    stream << "##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"Read Depth\">" << std::endl;
    stream << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">" << std::endl;
    stream << "##FORMAT=<ID=AD,Number=.,Type=Integer,Description=\"Allelic depths for the ref and alt alleles in the order listed\">" << std::endl;
    stream << "##FORMAT=<ID=SB,Number=4,Type=Integer,Description=\"Forward and reverse support for ref and alt alleles.\">" << std::endl;
    // We need this field to stratify on for VCF comparison. The info is in SB but vcfeval can't pull it out
    stream << "##FORMAT=<ID=XAAD,Number=1,Type=Integer,Description=\"Alt allele read count.\">" << std::endl;
    if(!contig_name.empty()) {
        // Announce the contig as well.
        stream << "##contig=<ID=" << contig_name << ",length=" << contig_size << ">" << std::endl;
    }
    stream << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t" << sample_name << std::endl;
}

vcflib::VariantCallFile* Genotyper::start_vcf(std::ostream& stream, const ReferenceIndex& index, const string& sample_name, const string& contig_name, size_t contig_size) {
    // Generate a vcf header. We can't make Variant records without a
    // VariantCallFile, because the variants need to know which of their
    // available info fields or whatever are defined in the file's header, so
    // they know what to output.
    // Handle length override if specified.
    std::stringstream headerStream;
    write_vcf_header(headerStream, sample_name, contig_name, contig_size > 0 ? contig_size : index.sequence.size());
    
    // Load the headers into a new VCF file object
    vcflib::VariantCallFile* vcf = new vcflib::VariantCallFile();
    std::string headerString = headerStream.str();
    assert(vcf->openForOutput(headerString));
    
    // Spit out the header
    stream << headerStream.str();
    
    // Give back the created VCF
    return vcf;
}

vector<vcflib::Variant> Genotyper::locus_to_variant(VG& graph, const Site& site, const ReferenceIndex& index, vcflib::VariantCallFile& vcf, const Locus& locus, const string& sample_name) {
    // Make a vector to fill in
    vector<vcflib::Variant> to_return;
    
    // Make a new variant
    vcflib::Variant variant;
    // Attach it to the VCF
    variant.setVariantCallFile(vcf);
    // Fake the quality
    variant.quality = 0;
    
    // Make sure we have stuff
    if(locus.allele_size() == 0) {
        throw runtime_error("Can't turn an empty genotype into VCF");
    }
    if(locus.allele(0).mapping_size() == 0) {
        throw runtime_error("Can't turn an empty allele into VCF");
    }
    
    // Get the superbubble    
    auto first_id = site.start.node->id();
    auto last_id = site.end.node->id();
    
    if(!index.byId.count(first_id) || !index.byId.count(last_id)) {
        // We need to be anchored to the primary path to make a variant
        cerr << "Warning: Superbubble endpoints not on reference!" << endl;
        // If not return no variant
        return to_return;
    }
    
    // The position we have stored for this start node is the first
    // position along the reference at which it occurs. Our bubble
    // goes forward in the reference, so we must come out of the
    // opposite end of the node from the one we have stored.
    auto referenceIntervalStart = index.byId.at(first_id).first + graph.get_node(first_id)->sequence().size();
    
    // The position we have stored for the end node is the first
    // position it occurs in the reference, and we know we go into
    // it in a reference-concordant direction, so we must have our
    // past-the-end position right there.
    auto referenceIntervalPastEnd = index.byId.at(last_id).first;
    
    // TODO: figure out how to handle superbubbles that come up backwards
    // relative to the primary reference.
    assert(referenceIntervalStart <= referenceIntervalPastEnd);
    
    // Get the string for the reference allele
    string refString = index.sequence.substr(referenceIntervalStart, referenceIntervalPastEnd - referenceIntervalStart);
    
    // And make strings for all the locus's alleles
    vector<string> allele_strings;
    
    for(size_t i = 0; i < locus.allele_size(); i++) {
        // Get the string for each allele
        allele_strings.push_back(allele_to_string(graph, locus.allele(i)));
    }
    
    // See if any alleles are empty
    bool empty_alleles = refString.empty();
    for(auto& allele : allele_strings) {
        if(allele == "") {
            empty_alleles = true;
        }
    }
    
    // Fix them up
    if(empty_alleles) {
        // Grab the character before our site
        string prefix = index.sequence.substr(referenceIntervalStart - 1, 1);
        for(auto& allele : allele_strings) {
            // Prepend it to every allele
            allele = prefix + allele;
        }
        // Also prepend it to the reference string
        refString = prefix + refString;
        
        // Budge the variant over
        referenceIntervalStart--;
    }
    
    // Make the ref allele
    create_ref_allele(variant, refString);
    
    // Make a vector of supports by assigned VCF alt number
    vector<Support> support_by_alt;
    
    // This maps from locus allele index to VCF record alt number
    vector<int> allele_to_alt;
    
    // This records the max alt number used. We might have more alts than
    // alleles if the reference was never considered as an allele.
    int max_alt_number = 0;
    
    for(size_t i = 0; i < locus.allele_size(); i++) {
        // For each allele
        
        // Add it/find its number if it already exists (i.e. is the ref)
        int alt_number = add_alt_allele(variant, allele_strings[i]);
        
        // See if it's a new max
        max_alt_number = max(max_alt_number, alt_number);
        
        // Remember what VCF alt number it got
        allele_to_alt.push_back(alt_number);
        
        if(i < locus.support_size()) {
            if(support_by_alt.size() <= alt_number) {
                // Make sure we have a slot to put the support in
                support_by_alt.resize(alt_number + 1);
            }
            // Put it there
            support_by_alt[alt_number] = locus.support(i);
        }
    }
    
    // Get the best genotype
    assert(locus.genotype_size() > 0);
    Genotype best_genotype = locus.genotype(0);
    // TODO: right now we only handle diploids
    assert(best_genotype.allele_size() == 2);
    
    // Compose the ML genotype
    variant.format.push_back("GT");
    auto& genotype_out = variant.samples[sample_name]["GT"];
    // Translate each allele to a VCF alt number, and put them in a string with the right separator
    genotype_out.push_back(to_string(allele_to_alt[best_genotype.allele(0)]) + (best_genotype.is_phased() ? "|" : "/")  +
        to_string(allele_to_alt[best_genotype.allele(1)]));
        
    // Put the total depth overall (not double-counting)
    string depth_string = std::to_string(locus.overall_support().forward() + locus.overall_support().reverse());
    variant.format.push_back("DP");
    variant.samples[sample_name]["DP"].push_back(depth_string);
    variant.info["DP"].push_back(depth_string); // We only have one sample, so variant depth = sample depth
    
    // Compose the allele-specific depth
    variant.format.push_back("AD");
    for(auto& support : support_by_alt) {
        // Add the forward and reverse support together and use that for AD for the allele.
        variant.samples[sample_name]["AD"].push_back(to_string(support.forward() + support.reverse()));
    }
    
    // Work out genotype log likelihoods
    // Make a vector to shuffle them into VCF order. Fill it with inf for impossible genotypes.
    vector<double> log_likelihoods((max_alt_number * (max_alt_number + 1)) / 2 + max_alt_number + 1,
        numeric_limits<double>::infinity());
    for(size_t i = 0; i < locus.genotype_size(); i++) {
        // For every genotype, calculate its VCF-order index based on the VCF allele numbers
        
        // TODO: we can only do diploids
        assert(locus.genotype(i).allele_size() == 2);
        
        // We first need the low and high alt numbers
        size_t low_alt = allele_to_alt.at(locus.genotype(i).allele(0));
        size_t high_alt = allele_to_alt.at(locus.genotype(i).allele(1));
        if(low_alt > high_alt) {
            // Flip them to be the right way around
            swap(low_alt, high_alt);
        }
        
        // Compute the position as (sort of) specified in the VCF spec
        size_t index = (high_alt * (high_alt + 1)) / 2 + low_alt;
        // Store the log likelihood
        log_likelihoods.at(index) = locus.genotype(i).log_likelihood();
#ifdef debug
        cerr << high_alt << "/" << low_alt << ": " << index << " = " << pb2json(locus.genotype(i)) << endl;
#endif
    }
    
    variant.format.push_back("PL");
    for(auto& log_likelihood : log_likelihoods) {
        // Add all the likelihood strings, normalizing against the best
        variant.samples[sample_name]["PL"].push_back(to_string(logprob_to_phred(log_likelihood - best_genotype.log_likelihood())));
    }
    
    // Set the variant position (now that we have budged it left if necessary
    variant.position = referenceIntervalStart + 1;
    
    // Return the variant, since we managed to make it
    to_return.push_back(variant);
    return to_return;
    
}

ReferenceIndex::ReferenceIndex(vg::VG& vg, std::string refPathName) {
    // Make sure the reference path is present
    assert(vg.paths.has_path(refPathName));
    
    // We're also going to build the reference sequence string
    std::stringstream refSeqStream;
    
    // What base are we at in the reference
    size_t referenceBase = 0;
    
    // What was the last rank? Ranks must always go up.
    int64_t lastRank = -1;
    
    for(auto mapping : vg.paths.get_path(refPathName)) {
    
        if(!byId.count(mapping.position().node_id())) {
            // This is the first time we have visited this node in the reference
            // path.
            
            // Add in a mapping.
            byId[mapping.position().node_id()] = 
                std::make_pair(referenceBase, mapping.position().is_reverse());
#ifdef debug
            std::cerr << "Node " << mapping.position().node_id() << " rank " << mapping.rank()
                << " starts at base " << referenceBase << " with "
                << vg.get_node(mapping.position().node_id())->sequence() << std::endl;
#endif
            
            // Make sure ranks are monotonically increasing along the path.
            assert(mapping.rank() > lastRank);
            lastRank = mapping.rank();
        }
        
        // Find the node's sequence
        std::string sequence = vg.get_node(mapping.position().node_id())->sequence();
        
        while(referenceBase == 0 && sequence.size() > 0 &&
            (sequence[0] != 'A' && sequence[0] != 'T' && sequence[0] != 'C' &&
            sequence[0] != 'G' && sequence[0] != 'N')) {
            
            // If the path leads with invalid characters (like "X"), throw them
            // out when computing reference path positions.
            
            // TODO: this is a hack to deal with the debruijn-brca1-k63 graph,
            // which leads with an X.
            
            std::cerr << "Warning: dropping invalid leading character "
                << sequence[0] << " from node " << mapping.position().node_id()
                << std::endl;
                
            sequence.erase(sequence.begin());
        }
        
        if(mapping.position().is_reverse()) {
            // Put the reverse sequence in the reference path
            refSeqStream << vg::reverse_complement(sequence);
        } else {
            // Put the forward sequence in the reference path
            refSeqStream << sequence;
        }
            
        // Say that this node appears here along the reference in this
        // orientation.
        byStart[referenceBase] = vg::NodeTraversal(
            vg.get_node(mapping.position().node_id()), mapping.position().is_reverse()); 
            
        // Whether we found the right place for this node in the reference or
        // not, we still need to advance along the reference path. We assume the
        // whole node (except any leading bogus characters) is included in the
        // path (since it sort of has to be, syntactically, unless it's the
        // first or last node).
        referenceBase += sequence.size();
        
        // TODO: handle leading bogus characters in calls on the first node.
    }
    
    // Create the actual reference sequence we will use
    sequence = refSeqStream.str();
    
    // Announce progress.
    std::cerr << "Traced " << referenceBase << " bp reference path " << refPathName << "." << std::endl;
    
    if(sequence.size() < 100) {
        std::cerr << "Reference sequence: " << sequence << std::endl;
    }
}


}


