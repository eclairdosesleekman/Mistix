import unittest
PRIORITIES = 32
def choose(items, current):
    ready = [(i, t) for i, t in enumerate(items) if i != current and t['state'] == 'ready']
    if not ready: return current if items[current]['state'] == 'ready' else None
    return min(ready, key=lambda pair: (-pair[1]['priority'], pair[1]['sequence']))[0]
class PrioritySchedulerTests(unittest.TestCase):
    def test_high_priority_wins(self):
        a=[{'state':'running','priority':10,'sequence':0},{'state':'ready','priority':20,'sequence':9},{'state':'ready','priority':31,'sequence':10}]
        self.assertEqual(choose(a,0),2)
    def test_fifo_within_priority(self):
        a=[{'state':'running','priority':16,'sequence':0},{'state':'ready','priority':7,'sequence':20},{'state':'ready','priority':7,'sequence':10}]
        self.assertEqual(choose(a,0),2)
    def test_single_runnable_thread_keeps_running(self):
        a=[{'state':'ready','priority':3,'sequence':1}]
        self.assertEqual(choose(a,0),0)
    def test_priority_domain(self):
        self.assertEqual(PRIORITIES,32); self.assertEqual(max(range(PRIORITIES)),31)
if __name__ == '__main__': unittest.main()
