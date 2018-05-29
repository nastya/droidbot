#!/usr/bin/python2
import json
import sys
from subprocess import check_output
#from state_transition_graph import TransitionGraph
import os

abspath = os.path.abspath(__file__)
dname = os.path.dirname(abspath)
sys.path.append(dname + "/static_deps")
import known_libs


class DynamicAPIModel:
    """
    The DynamicAPIModel (DAPIM) of an app
    uses the following entities: app state, events (stimulus), app reaction
    """
    def __init__(self, input_path=None, used_packages=None):
        #super(DynamicAPIModel, self).__init__(input_path)
        self.event_path = input_path + 'events' if input_path is not None else None
        self.used_packages = used_packages if used_packages is not None else []
        self.get_events_match()
        self.get_unique_states()
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
                placed_event = event + '~' + placed_event
                #if placed_event in d_model_placed_events[s_state]:
                    #placed_event += '1'
                d_model_placed_events[s_state][placed_event] = self.d_model[s_state][event]
        return d_model_placed_events


    def get_events_match(self):
        self.events_match = {}
        self.event_tags_list = []
        for root, _, events in os.walk(self.event_path):
            for event in sorted(events):
                if event[-5:] != '.json':
                    continue
                event_tag = event[6:-5]
                self.event_tags_list.append(event_tag)
                self.events_match[event_tag] = \
                    json.loads(open(self.event_path + '/' + event).read())

    def get_unique_states(self):
        self.unique_states = []
        #for event in self.data:
            #if event['startState'] not in self.unique_states:
                #self.unique_states.append(event['startState'])
            #if event['endState'] not in self.unique_states:
                #self.unique_states.append(event['endState'])
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

    @staticmethod
    def app_method(method, used_packages):
        for p in used_packages:
            if p in method:
                return True
        return False

    @staticmethod
    def parse_trace_file_appfunc(filename, used_packages):
        threads_api = {}
        if not os.path.isfile(filename):
            #print 'Tracefile ' + filename + ' not found'
            return {}
        if os.stat(filename).st_size == 0:
            #print 'Tracefile ' + filename + ' is empty'
            return {}
        try:
            full_trace = check_output([dname + "/dmtracedump/TraceDumpMod", filename])
        except:
            #print 'dmtracedump failed to parse trace file ', filename
            return {}
        dumping_threads = {}
        for line in full_trace.split("\n"):
            parsed_line = line.split()
            if len(parsed_line) < 10:
                break #this was the ending line

            threadId = parsed_line[0]
            time = parsed_line[1]
            action = parsed_line[2]
            method = parsed_line[8]
            args = parsed_line[9]
            if (len(parsed_line) >= 11):
                usedfile = parsed_line[10]
            method = method.replace('/', '.')
            if DynamicAPIModel.app_method(method, used_packages):
                if action == '0':
                    if not threadId in dumping_threads or dumping_threads[threadId] == '':
                        dumping_threads[threadId] = method
                    if not threadId in threads_api:
                        threads_api[threadId] = []
                else:
                    if dumping_threads[threadId] == method:
                        dumping_threads[threadId] = ''
            if DynamicAPIModel.app_method(method, used_packages) and action == '0':
                threads_api[threadId].append(method)
        return threads_api


    @staticmethod
    def parse_trace_file_api(filename, used_packages):
        threads_api = {}
        if not os.path.isfile(filename):
            #print 'Tracefile ' + filename + ' not found'
            return {}
        if os.stat(filename).st_size == 0:
            #print 'Tracefile ' + filename + ' is empty'
            return {}
        try:
            full_trace = check_output([dname + "/dmtracedump/TraceDumpMod", filename])
        except:
            #print 'dmtracedump failed to parse trace file ', filename
            return {}
        dumping_threads = {}
        thread_in_api = {}
        for line in full_trace.split("\n"):
            parsed_line = line.split()
            if len(parsed_line) < 10:
                break #this was the ending line
            threadId = parsed_line[0]
            time = parsed_line[1]
            action = parsed_line[2]
            method = parsed_line[8]
            method = method.replace('/', '.')
            args = parsed_line[9]
            if (len(parsed_line) >= 11):
                usedfile = parsed_line[10]
            if DynamicAPIModel.app_method(method, used_packages):
                if action == '0':
                    if not threadId in dumping_threads or dumping_threads[threadId] == '':
                        dumping_threads[threadId] = method
                        thread_in_api[threadId] = ''
                    if not threadId in threads_api:
                        threads_api[threadId] = []
                else:
                    if dumping_threads[threadId] == method:
                        dumping_threads[threadId] = ''
                        thread_in_api[threadId] = ''
            else:
                if threadId in dumping_threads and dumping_threads[threadId] != '' and \
                                                action == '0' and thread_in_api[threadId] == '':
                    threads_api[threadId].append(method)
                    thread_in_api[threadId] = method
                if threadId in dumping_threads and dumping_threads[threadId] != '' and \
                                                action == '1' and thread_in_api[threadId] == method:
                    thread_in_api[threadId] = ''
        return threads_api


    def build_dynamic_model(self):
        self.d_model = {}
        for event_tag in self.events_match:
            event = self.events_match[event_tag]
            s_state = event['start_state']
            #event_tag = self.event_tags_list[int(event["id"]) - 1]
            if not s_state in self.d_model:
                self.d_model[s_state] = {}
            self.d_model[s_state][event_tag] = {
                'reaction': self.event_trace[event_tag],
                'endState': event['stop_state']
            }

    def get_trace_info(self):
        self.event_trace = {}
        for event_tag in self.event_tags_list:
            self.event_trace[event_tag] = DynamicAPIModel.parse_trace_file_api(self.event_path + \
                '/event_trace_' + event_tag + '.trace', self.used_packages)

def isLibraryClass(classname):
    package_method = False
    for package in known_libs.known_libs:
        package_name = "L" + package + "/"
        package_name = package_name.replace(".", "/")
        if package_name in classname:
            package_method = True
            break
    return package_method

def get_used_packages(apk_name):
    from androguard.core.bytecodes.apk import APK
    a = APK(apk_name)
    from androguard.misc import AnalyzeDex
    d, dx = AnalyzeDex(a.get_dex(), raw=True)
    p_list = []
    for cl in d.get_classes():
        if not isLibraryClass(cl.get_name()):
            splitted = cl.get_name().split('/')
            short_name = splitted[0][1:]
            if len(splitted) >= 2:
                short_name += '.' + splitted[1]
            if len(splitted) > 3:
                short_name += '.' + splitted[2]

            if not short_name in p_list:
                p_list.append(short_name)
    return p_list

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print 'Specify path to droidbot output directory and .apk file'
        sys.exit(0)
    input_path = sys.argv[1]
    input_path = os.path.abspath(input_path) + '/'
    apk_name = sys.argv[2]
    used_packages = get_used_packages(apk_name)

    graph = DynamicAPIModel(input_path, used_packages)

    #data = graph.to_json()
    print json.dumps(graph.place_event_values(), indent = 4)
    #DynamicAPIModel.parse_trace_file_api("/home/nastya/droidbot/droidbot_out_test/events/event_trace_2016-10-04_184336.trace", "com.alexcruz.papuhwalls")
