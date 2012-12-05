/*
 * =====================================================================================
 *
 *       Filename:  graph.cc
 *
 *    Description:  Graph BusyBee loop for each server
 *
 *        Version:  1.0
 *        Created:  Tuesday 16 October 2012 03:03:11  EDT
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Ayush Dubey (), dubey@cs.cornell.edu
 *   Organization:  Cornell University
 *
 * =====================================================================================
 */

//C
#include <cstdlib>

//C++
#include <iostream>

//STL
#include <vector>
#include <unordered_map>

//po6
#include <po6/net/location.h>

//e
#include <e/buffer.h>

//Busybee
#include "busybee_constants.h"

//Weaver
#include "graph.h"
#include "../message/message.h"

#define IP_ADDR "127.0.0.1"
#define PORT_BASE 5200

class mytuple
{
	public:
		uint16_t port;
		uint32_t counter; //prev req id
		int num; //number of requests
		bool reachable;

		mytuple ()
		{
			port = 0;
			counter = 0;
			num = 0;
			reachable = false;
		}

		mytuple (uint16_t p, uint32_t c, int n)
			: port(p)
			, counter(c)
			, num(n)
			, reachable (false)
		{
		}

};

int order, port;

void
runner (db::graph* G)
{
	busybee_returncode ret;
	int sent = 0;
	po6::net::location central (IP_ADDR, PORT_BASE);
	po6::net::location sender (IP_ADDR, PORT_BASE);
	message::message msg (message::ERROR);

	db::element::node *n;
	db::element::edge *e;
	db::element::meta_element *n1, *n2;
	void *mem_addr1, *mem_addr2;
	po6::net::location *local, *remote;
	uint32_t direction;
	uint16_t to_port, from_port;
	uint32_t req_counter, prev_req_counter;

	std::unordered_map<uint16_t, std::vector<size_t>> msg_batch;
	std::vector<size_t> src_nodes;
	std::vector<size_t>::iterator src_iter;
	bool reached = false;
	bool send_msg = false;
	std::unordered_map<uint32_t, mytuple> outstanding_req;
	uint32_t local_req_counter = 0;
	std::unordered_map<uint32_t, mytuple> pending_batch;
	uint32_t local_batch_req_counter = 0;
	mytuple *out_req;
	bool reachable_reply;
	uint32_t num_nack, temp1;

	uint32_t code;
	enum message::msg_type mtype;

	uint32_t loop_count = 0;
	while (1)
	{
		if ((ret = G->bb.recv (&sender, &msg.buf)) != BUSYBEE_SUCCESS)
		{
			std::cerr << "msg recv error: " << ret << std::endl;
			continue;
		}
		msg.buf->unpack_from (BUSYBEE_HEADER_SIZE) >> code;
		mtype = (enum message::msg_type) code;
		switch (mtype)
		{
			case message::NODE_CREATE_REQ:
				n = G->create_node (0);
				msg.change_type (message::NODE_CREATE_ACK);
				if (msg.prep_create_ack ((size_t) n) != 0) 
				{
					continue;
				}
				if ((ret = G->bb.send (central, msg.buf)) != BUSYBEE_SUCCESS) 
				{
					std::cerr << "msg send error: " << ret << std::endl;
					continue;
				}
				break;

			case message::EDGE_CREATE_REQ:
				if (msg.unpack_edge_create (&mem_addr1, &mem_addr2, &remote, &direction) != 0)
				{
					continue;
				}
				local = new po6::net::location (IP_ADDR, port);
				n1 = new db::element::meta_element (*local, 0, UINT_MAX,
					mem_addr1);
				n2 = new db::element::meta_element (*remote, 0, UINT_MAX,
					mem_addr2);
				
				e = G->create_edge (n1, n2, (uint32_t) direction, 0);
				msg.change_type (message::EDGE_CREATE_ACK);
				if (msg.prep_create_ack ((size_t) e) != 0) 
				{
					continue;
				}
				if ((ret = G->bb.send (central, msg.buf)) != BUSYBEE_SUCCESS) 
				{
					std::cerr << "msg send error: " << ret << std::endl;
					continue;
				}
				break;

			
			case message::REACHABLE_PROP:
				reached = false;
				send_msg = false;
				num_nack = 0;
				src_nodes = msg.unpack_reachable_prop (&from_port, 
													   &mem_addr2, 
													   &to_port,
													   &req_counter, 
													   &prev_req_counter);
				msg_batch.clear();
				local_batch_req_counter++;

				for (src_iter = src_nodes.begin(); src_iter < src_nodes.end();
					 src_iter++)
				{
				static int node_ctr = 0;
				//no error checking needed here
				n = (db::element::node *) (*src_iter);
				//TODO mem leak! Remove old properties
				if (!G->mark_visited (n, req_counter))
				{
					std::vector<db::element::meta_element>::iterator iter;
					for (iter = n->out_edges.begin(); iter < n->out_edges.end();
						 iter++)
					{
						send_msg = true;
						db::element::edge *nbr = (db::element::edge *)
												  iter->get_addr();
						if (nbr->to.get_addr() == mem_addr2 &&
							nbr->to.get_port() == to_port)
						{ //Done! Send msg back to central server
							reached = true;
							break;
						} else
						{ //Continue propagating reachability request
							msg_batch[nbr->to.get_port()].push_back
								((size_t)nbr->to.get_addr());
						}
					}
				} //end if visited
				if (reached)
				{
					break;
				}
				} //end src_nodes loop
				
				//send messages
				if (reached)
				{ //need to send back ack
					msg.change_type (message::REACHABLE_REPLY);
					msg.prep_reachable_rep (prev_req_counter, true);
					remote = new po6::net::location (IP_ADDR, 
													 from_port);
					if ((ret = G->bb.send (*remote, msg.buf)) != BUSYBEE_SUCCESS)
					{
						std::cerr << "msg send error: " << ret << std::endl;
					}
				} else if (send_msg)
				{ //need to send batched msges onwards
					outstanding_req[local_batch_req_counter].port = from_port;
					outstanding_req[local_batch_req_counter].counter =
						prev_req_counter;
					outstanding_req[local_batch_req_counter].num = 0;
					std::unordered_map<uint16_t, std::vector<size_t>>::iterator 
						loc_iter;
					for (loc_iter = msg_batch.begin(); loc_iter !=
						 msg_batch.end(); loc_iter++)
					{
						static int loop = 0;
						remote = new po6::net::location (IP_ADDR,
								 						 loc_iter->first);
						msg.change_type (message::REACHABLE_PROP);
						msg.prep_reachable_prop (loc_iter->second,
												 G->myloc.port, 
												 (size_t)mem_addr2, 
												 to_port,
												 req_counter,
												 (++local_req_counter));
						if ((ret = G->bb.send (*remote, msg.buf)) !=
							BUSYBEE_SUCCESS)
						{
							std::cerr << "msg send error: " << ret <<
							std::endl;
						}
						//adding this as a pending request
						outstanding_req[local_batch_req_counter].num++;
						pending_batch[local_req_counter].num =
							loc_iter->second.size();
						pending_batch[local_req_counter].counter =
							local_batch_req_counter;
					}
					msg_batch.clear();	
				} else
				{ //need to send back nack
					msg.change_type (message::REACHABLE_REPLY);
					msg.prep_reachable_rep (prev_req_counter, false);
					remote = new po6::net::location (IP_ADDR, from_port);
					if ((ret = G->bb.send (*remote, msg.buf)) !=
						BUSYBEE_SUCCESS)
					{
						std::cerr << "msg send error: " << ret << std::endl;
					}
					num_nack--;
				}
				break;

			case message::REACHABLE_REPLY:
				msg.unpack_reachable_rep (&req_counter, &reachable_reply);
				temp1 = pending_batch[req_counter].counter;
				pending_batch.erase (req_counter);
				--outstanding_req[temp1].num;
				from_port = outstanding_req[temp1].port;
				prev_req_counter = outstanding_req[temp1].counter;
				/*
				 * check if this is the last expected reply for this batched request
				 * and we got all negative replies till now
				 * or this is a positive reachable reply
				 */
				if (((outstanding_req[temp1].num == 0) || reachable_reply)
					&& !outstanding_req[temp1].reachable)
				{
					outstanding_req[temp1].reachable |= reachable_reply;
					msg.prep_reachable_rep (prev_req_counter, reachable_reply);
					remote = new po6::net::location (IP_ADDR, from_port);
					if ((ret = G->bb.send (*remote, msg.buf)) != BUSYBEE_SUCCESS)
					{
						std::cerr << "msg send error: " << ret << std::endl;
					}
				}
				if (outstanding_req[temp1].num == 0)
				{
					outstanding_req.erase (temp1);
				}
				break;

			default:
				std::cerr << "unexpected msg type " << code << std::endl;
		
		} //end switch

	} //end while

}

int
main (int argc, char* argv[])
{
	if (argc != 2) 
	{
		std::cerr << "Usage: " << argv[0] << " <order> " << std::endl;
		return -1;
	}

	std::cout << "Testing Weaver" << std::endl;
	
	order = atoi (argv[1]);
	port = PORT_BASE + order;

	db::graph G (IP_ADDR, port);
	
	runner (&G);

	return 0;
}