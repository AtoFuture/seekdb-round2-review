"""Provenance regressions for a smaller self-contained learning submission."""
import copy
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'scripts/check-learning-material.py'
spec = importlib.util.spec_from_file_location('learning_material', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.archive = {
            'conversations': [{'id': 'u1', 'role': 'user', 'content': 'measure T/B/M'}],
            'experiments': [{'id': 'e1', 'source': 'tool', 'output': 'failed'},
                            {'id': 'e2', 'source': 'tool', 'output': 'passed after repair'}],
            'agent_observations': [{'id': 'a1', 'source': 'agent', 'content': 'explanation'}]}
        self.current = copy.deepcopy(self.archive)
        self.current['experiments'] = self.current['experiments'][1:]

    def test_exact_record_selection_passes(self):
        module.verify_selection(self.current, self.archive)

    def test_appendix_keeps_original_immutable_and_requires_all_user_records(self):
        before = copy.deepcopy(self.archive)
        extra = {'conversations': [{'id': 'u2', 'source': 'user', 'content': 'actual feedback'}],
                 'experiments': [], 'agent_observations': []}
        extended = module.append_records(self.archive, extra)
        self.assertEqual(self.archive, before)
        current = copy.deepcopy(self.current)
        current['conversations'] += extra['conversations']
        module.verify_selection(current, extended)
        current['conversations'].pop()
        with self.assertRaises(ValueError):
            module.verify_selection(current, extended)

    def test_appendix_cannot_replace_record_or_repeat_id(self):
        for rows in [[{'id': 'u1', 'content': 'replacement'}],
                     [{'id': 'u2'}, {'id': 'u2'}]]:
            extra = {'conversations': rows, 'experiments': [], 'agent_observations': []}
            with self.assertRaises(ValueError):
                module.append_records(self.archive, extra)

    def test_cannot_rewrite_or_invent_user_explanation(self):
        self.current['conversations'][0]['content'] = 'I independently understood the implementation'
        with self.assertRaises(ValueError):
            module.verify_selection(self.current, self.archive)

    def test_cannot_hide_existing_user_statement(self):
        self.current['conversations'] = []
        with self.assertRaises(ValueError):
            module.verify_selection(self.current, self.archive)

    def test_cannot_duplicate_results_or_relabel_agent_as_user(self):
        self.current['experiments'] *= 2
        with self.assertRaises(ValueError):
            module.verify_selection(self.current, self.archive)
        self.current = copy.deepcopy(self.archive)
        self.current['agent_observations'][0]['source'] = 'user'
        with self.assertRaises(ValueError):
            module.verify_selection(self.current, self.archive)

    def test_cannot_change_archived_results_or_add_unrecorded_experiment(self):
        self.current['experiments'][0]['output'] = 'passed before repair'
        with self.assertRaises(ValueError):
            module.verify_selection(self.current, self.archive)
        self.current['experiments'] = [{'id': 'e3', 'source': 'tool', 'output': 'invented'}]
        with self.assertRaises(ValueError):
            module.verify_selection(self.current, self.archive)


if __name__ == '__main__':
    unittest.main()
