#!/usr/bin/python2
import json
import os
import sys
from subprocess import check_output

abspath = os.path.abspath(__file__)
dname = os.path.dirname(abspath)

sys.path.append(dname)
state_transition_graph_flag = True
try:
	from state_transition_graph import TransitionGraph
except:
	state_transition_graph_flag = False
sys.path.append(dname + "/static_deps")
import known_libs
from datetime import datetime


class DynamicAPIModel:
	"""
	The DynamicAPIModel (DAPIM) of an app
	uses the following entities: app state, events (stimulus), app reaction
	app reaction is given in terms of droidbox events
	"""
	def __init__(self, input_path=None):
		self.timeline = {}
		self.input_path = input_path
		self.read_droidbox_log()		
		self.event_path = input_path + 'events' if input_path is not None else None
		self.get_events_match()
		#self.get_unique_states()
		self.get_trace_info()
		self.build_dynamic_model()

	def place_event_values(self):
		d_model_placed_events = {}
		for s_state in self.d_model:
			d_model_placed_events[s_state] = {}
			for event in self.d_model[s_state]:
				placed_event = self.events_match[event]["event"]["event_type"]
				if placed_event == "touch":
					placed_event += '(' + str(self.events_match[event]["event"]["x"]) + ',' + \
						str(self.events_match[event]["event"]["y"]) + ')'
				placed_event = str(event) + '~' + placed_event
				#if placed_event in d_model_placed_events[s_state]:
					#placed_event += '1'
				d_model_placed_events[s_state][placed_event] = self.d_model[s_state][event]
		return d_model_placed_events


	def get_events_match(self):
		self.events_match = {}
		self.event_tags_list = []
		#print self.startTime
		for root, _, events in os.walk(self.event_path):
			for event in sorted(events):
				if event[-5:] != '.json':
					continue
				event_tag = event[6:-5]
				#print event_tag
				event_tag_datetime = datetime.strptime(event_tag, "%Y-%m-%d_%H%M%S")
				if event_tag_datetime < self.startTime:
					event_tag_seconds = - (self.startTime - event_tag_datetime).seconds
				else:
					event_tag_seconds = (event_tag_datetime - self.startTime).seconds
				#print event_tag_seconds
				try:
					self.events_match[event_tag_seconds] = \
						json.loads(open(self.event_path + '/' + event).read())
					self.event_tags_list.append(event_tag_seconds)
				except:
					pass
		#print self.event_tags_list

	def get_unique_states(self):
		self.unique_states = []
		
		# for old version
		#for event in self.data:
			#if event['startState'] not in self.unique_states:
				#self.unique_states.append(event['startState'])
			#if event['endState'] not in self.unique_states:
				#self.unique_states.append(event['endState'])
		# for new version
		for root, _, events in os.walk(self.event_path):
			for event in sorted(events):
				if event[-5:] != '.json':
					continue
				event_info = json.loads(open(self.event_path + '/' + event).read())
				if event_info['start_state'] not in self.unique_states:
					self.unique_states.append(event_info['start_state'])
				if event_info['stop_state'] not in self.unique_states:
					self.unique_states.append(event_info['stop_state'])
		pass

	def build_dynamic_model(self):
		self.d_model = {}
		if state_transition_graph_flag:
			try:
				tg = TransitionGraph(self.input_path)
				for event in tg.data:
					#print event
					s_state = event['startState']
					event_tag = self.event_tags_list[int(event["id"]) - 1]
					if not s_state in self.d_model:
						self.d_model[s_state] = {}
					self.d_model[s_state][event_tag] = {
						'reaction': self.event_trace[event_tag],
						'endState': event['endState']
					}
				return
			except Exception as e:
				pass
		try:
			for event_tag in self.event_tags_list:
				s_state = self.events_match[event_tag]["start_state"]
				if not s_state in self.d_model:
					self.d_model[s_state] = {}
				self.d_model[s_state][event_tag] = {
					'reaction': self.event_trace[event_tag],
					'endState': self.events_match[event_tag]["stop_state"]
				}
		except:
			print 'Wrong version of droidbot. Switch to droidbox_old branch.'
			

	def get_trace_info(self):
		self.event_trace = {}
		self.event_tags_list.sort()
		timeline_index = 0
		timeline_keys = sorted(self.timeline.keys())
		for i in range(len(self.event_tags_list)):
			event_tag = self.event_tags_list[i]
			self.event_trace[event_tag] = []
			while timeline_index < len(timeline_keys) and i < len(self.event_tags_list) -1 and \
					timeline_keys[timeline_index] < self.event_tags_list[i + 1]:
				self.event_trace[event_tag].append(self.timeline[timeline_keys[timeline_index]])
				timeline_index += 1

	def read_droidbox_log(self):
		droidbox_log = json.loads(open(self.input_path + 'analysis.json', 'r').read())
		self.startTime = datetime.strptime(droidbox_log["analysisStart"], "%Y-%m-%d %H:%M:%S.%f")
		#print self.startTime
		for entry in droidbox_log["sensitiveBehaviors"]:
			self.timeline[float(entry['time'])] = {}
			self.timeline[float(entry['time'])]['type'] = entry['type']
			self.timeline[float(entry['time'])]['detail'] = entry['detail']
			self.timeline[float(entry['time'])]['process'] = entry['process']
		

if __name__ == '__main__':
	if len(sys.argv) < 2:
		print 'Specify path to droidbot output directory'
		sys.exit(0)
	input_path = sys.argv[1]
	input_path = os.path.abspath(input_path) + '/'

	graph = DynamicAPIModel(input_path)

	print json.dumps(graph.place_event_values(), indent = 4)
 
