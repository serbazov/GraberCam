import unittest
from scripts.report_roi_switch import summarize


def frame(index, zone, expected=-1, block=0, test='live_pipeline'):
    return dict(test=test, mode='discard', group=2, block=block, index=index,
                expected=expected, observed=zone, frame_id=index+1,
                device_timestamp=index*1000, ready_us=index*1000,
                copy_us=0, score=.99, margin=.1)


def switch(first):
    return dict(test='live_pipeline',mode='discard',group=2,block=0,target=1,
                issue_frame=1,begin_us=1000,after_stop_us=1000,
                after_write_us=2000,after_start_us=2000,
                first_target_us=first,previous_frame_us=-1,stale_frames=1)


class ReportTests(unittest.TestCase):
    def test_actual_zone_runs_and_noncyclic_transitions(self):
        frames = [frame(i,z) for i,z in enumerate([0,0,1,1,2,2,3,3])]
        r = summarize(frames,[])[0]
        self.assertEqual(r['zone_run_p50'],2)
        self.assertEqual(r['noncyclic_transitions'],0)
        self.assertEqual(r['dropped'],0)
        self.assertAlmostEqual(r['capture_fps'],1000)
        self.assertAlmostEqual(r['actual_switches_s'],500)
        self.assertAlmostEqual(r['window_switches_s'],3/.007)
        frames[4]['observed']=3
        self.assertGreater(summarize(frames,[])[0]['noncyclic_transitions'],0)

    def test_unfinished_command_not_used_for_latency(self):
        r = summarize([frame(i,i//2) for i in range(4)], [switch(3000),switch(-1)])[0]
        self.assertEqual(r['confirmed_switches'],1)
        self.assertEqual(r['first_target_ms_p50'],2)
        self.assertEqual(r['switches'],2)

    def test_reset_ids_between_blocks_not_counted_as_drops(self):
        rows = [frame(0,0,0,0,'baseline'),frame(1,0,0,0,'baseline'),
                frame(0,1,1,1,'baseline'),frame(1,1,1,1,'baseline')]
        rows[2]['frame_id']=100
        rows[3]['frame_id']=101
        rows[2]['ready_us']=10000
        rows[3]['ready_us']=11000
        r = summarize(rows,[])[0]
        self.assertEqual(r['dropped'],0)
        self.assertEqual(r['wrong_zone'],0)
        self.assertEqual(r['capture_fps'],1000)
        rows[0]['observed']=-1
        rows[1]['observed']=2
        r = summarize(rows,[])[0]
        self.assertEqual(r['unknown'],1)
        self.assertEqual(r['wrong_zone'],1)


if __name__ == '__main__':
    unittest.main()
