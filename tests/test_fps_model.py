"""Verify model fitting/exclusion and independent prediction with known timings."""
import importlib.util
import csv
import json
import tempfile
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fps_model', ROOT/'scripts/fit_fps_model.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ModelTests(unittest.TestCase):
    def training_rows(self):
        rows = []
        for fmt, a, b, c, d in [('Mono8', 2, 77.5, 1, 24.65), ('Mono10', 2.47619, 96.1, 1.02, 30.5)]:
            for width in (9344, 256):
                for height in (2, 100, 1000):
                    for exposure in (27, 1000, 5000):
                        period = max(a*height+b, c*exposure+d)
                        rows.append(dict(format=fmt, width=width, height=height, exposure=exposure,
                                         mode='discard', delivered_fps=1e6/period, frames=100,
                                         dropped=0, timeouts=0, disorder=0, dataset='training'))
        return rows

    def test_fit_excludes_copy_and_losses(self):
        rows = self.training_rows()
        rows += [dict(rows[0], dropped=10, delivered_fps=1),
                 dict(rows[0], mode='copy', delivered_fps=1)]
        models = module.fit_models(rows)
        self.assertAlmostEqual(models['Mono8']['row_us'], 2, places=8)
        self.assertAlmostEqual(models['Mono8']['readout_overhead_us'], 77.5, places=8)
        self.assertAlmostEqual(models['Mono8']['exposure_overhead_us'], 24.65, places=8)
        self.assertAlmostEqual(models['Mono10']['row_us'], 2.47619, places=8)
        self.assertAlmostEqual(models['Mono10']['exposure_scale'], 1.02, places=8)
        self.assertAlmostEqual(models['Mono10']['exposure_overhead_us'], 30.5, places=8)

    def test_predicts_unseen_height_exposure_and_width(self):
        models = module.fit_models(self.training_rows())
        for width in (9008, 64):
            for height, exposure in ((777, 300), (31, 2300)):
                expected = 1e6/max(2*height+77.5, exposure+24.65)
                row = dict(format='Mono8', width=width, height=height, exposure=exposure,
                           mode='discard', delivered_fps=expected, frames=100, dataset='holdout')
                scored = module.score_rows(models, [row], 'validation')
                self.assertLess(scored[0]['abs_error_pct'], 1e-8)
                self.assertEqual(scored[0]['split'], 'validation')

    def test_csv_fallback_matches_json_fit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            rows = self.training_rows()
            with (root/'results.csv').open('w') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            csv_models = module.fit_models(module.load_rows([root]))
            (root/'results.json').write_text(json.dumps(rows))
            json_models = module.fit_models(module.load_rows([root]))
            self.assertEqual(csv_models, json_models)

    def test_rejects_underdetermined_regression(self):
        with self.assertRaises(ValueError):
            module.linear_fit([(2, 10), (2, 20)])


if __name__ == '__main__':
    unittest.main()
